#include <core/system.h>
#include <core/panic.h>
#include <mm/mm.h>
#include <mm/vm.h>


int debug_kmalloc = 0;

typedef struct {
    uint32_t addr : 28; /* Offseting (1GiB), 4-bytes aligned objects */
    uint32_t free : 1;  /* Free or not flag */
    uint32_t size : 26; /* Size of one object can be up to 256MiB */
    uint32_t next : 25; /* Index of the next node */
} __packed vmm_node_t;

size_t kvmem_used;
size_t kvmem_obj_cnt;

#define KVMEM_BASE       ARCH_KVMEM_BASE
#define KVMEM_NODES_SIZE ARCH_KVMEM_NODES_SIZE
#define KVMEM_NODES      (KVMEM_BASE - KVMEM_NODES_SIZE)

#define NODE_ADDR(node) (KVMEM_BASE + ((node).addr) * 4)
#define NODE_SIZE(node) (((node).size) * 4)
#define LAST_NODE_INDEX (100000)
#define MAX_NODE_SIZE   ((1UL << 26) - 1)

struct vmr kvmem_nodes = {
    .base  = KVMEM_NODES,
    .size  = KVMEM_NODES_SIZE,
    .flags = VM_KRW,
};

vmm_node_t *nodes = (vmm_node_t *) KVMEM_NODES;
void kvmem_setup()
{
    /* We start by mapping the space used for nodes into physical memory */
    //vm_map(VMM_NODES, VMM_NODES_SIZE, VM_KRW);
    vm_map(&kvmem_nodes);

    /* Now we have to clear it */
    memset((void *) KVMEM_NODES, 0, KVMEM_NODES_SIZE);

    /* Setting up initial node */
    nodes[0] = (vmm_node_t){0, 1, -1, LAST_NODE_INDEX};
}

uint32_t first_free_node = 0;
uint32_t get_node()
{
    for (unsigned i = first_free_node; i < LAST_NODE_INDEX; ++i) {
        if (!nodes[i].size) {
            return i;
        }
    }

    panic("Can't find an unused node");
    return -1;
}

void release_node(uint32_t i)
{
    nodes[i] = (vmm_node_t){0};
}

uint32_t get_first_fit_free_node(uint32_t size)
{
    unsigned i = first_free_node;

    while (!(nodes[i].free && nodes[i].size >= size)) {
        if (nodes[i].next == LAST_NODE_INDEX)
            panic("Can't find a free node");
        i = nodes[i].next;
    }

    return i;
}

void print_node(unsigned i)
{
    printk("Node[%d]\n", i);
    printk("   |_ Addr   : %x\n", NODE_ADDR(nodes[i]));
    printk("   |_ free?  : %s\n", nodes[i].free?"yes":"no");
    printk("   |_ Size   : %d B [ %d KiB ]\n",
        NODE_SIZE(nodes[i]), NODE_SIZE(nodes[i])/1024);
    printk("   |_ Next   : %d\n", nodes[i].next );
}

void *(kmalloc)(size_t size)
{
    //printk("kmalloc(%d)\n", size);
    size = (size + 3)/4;    /* size in 4-bytes units */

    /* Look for a first fit free node */
    unsigned i = get_first_fit_free_node(size);

    /* Mark it as used */
    nodes[i].free = 0;

    /* Split the node if necessary */
    if (nodes[i].size > size) {
        unsigned n = get_node();
        nodes[n] = (vmm_node_t) {
            .addr = nodes[i].addr + size,
            .free = 1,
            .size = nodes[i].size - size,
            .next = nodes[i].next
        };

        nodes[i].next = n;
        nodes[i].size = size;
    }

    kvmem_used += NODE_SIZE(nodes[i]);
    kvmem_obj_cnt++;

    vaddr_t map_base = UPPER_PAGE_BOUNDARY(NODE_ADDR(nodes[i]));
    vaddr_t map_end  = UPPER_PAGE_BOUNDARY(NODE_ADDR(nodes[i]) + NODE_SIZE(nodes[i]));
    size_t  map_size = (map_end - map_base)/PAGE_SIZE;

    if (map_size) {
        struct vmr vmr = {
            .paddr = 0,
            .base  = map_base,
            .size  = map_size * PAGE_SIZE,
            .flags = VM_KRW,
        };

        vm_map(&vmr);
    }

    return (void *) NODE_ADDR(nodes[i]);
}

void (kfree)(void *_ptr)
{
    //printk("kfree(%p)\n", _ptr);
    uintptr_t ptr = (uintptr_t) _ptr;

    if (ptr < KVMEM_BASE)  /* That's not even allocatable */
        return;

    /* Look for the node containing _ptr -- merge sequential free nodes */
    size_t cur_node = 0, prev_node = 0;

    while (ptr != NODE_ADDR(nodes[cur_node])) {
        /* check if current and previous node are free */
        if (cur_node && nodes[cur_node].free && nodes[prev_node].free) {
            /* check for overflow */
            if ((uintptr_t) (nodes[cur_node].size + nodes[prev_node].size) <= MAX_NODE_SIZE) {
                nodes[prev_node].size += nodes[cur_node].size;
                nodes[prev_node].next  = nodes[cur_node].next;
                release_node(cur_node);
                cur_node = nodes[prev_node].next;
                continue;
            }
        }

        prev_node = cur_node;
        cur_node = nodes[cur_node].next;

        if (cur_node == LAST_NODE_INDEX)  /* Trying to free unallocated node */
            goto done;
    }

    if (nodes[cur_node].free)  /* Node is already free, dangling pointer? */
        goto done;

    /* First we mark our node as free */
    nodes[cur_node].free = 1;

    kvmem_used -= NODE_SIZE(nodes[cur_node]);
    if (debug_kmalloc)
        printk("NODE_SIZE %d\n", NODE_SIZE(nodes[cur_node]));
    kvmem_obj_cnt--;

    /* Now we merge all free nodes ahead -- except the last node */
    while (nodes[cur_node].next < LAST_NODE_INDEX && nodes[cur_node].free) {
        /* check if current and previous node are free */
        if (cur_node && nodes[cur_node].free && nodes[prev_node].free) {
            /* check for overflow */
            if ((uintptr_t) (nodes[cur_node].size + nodes[prev_node].size) <= MAX_NODE_SIZE) {
                nodes[prev_node].size += nodes[cur_node].size;
                nodes[prev_node].next  = nodes[cur_node].next;
                release_node(cur_node);
                cur_node = nodes[prev_node].next;
                continue;
            }
        }

        prev_node = cur_node;
        cur_node = nodes[cur_node].next;
    }

    cur_node = 0;
    while (nodes[cur_node].next < LAST_NODE_INDEX) {
        if (nodes[cur_node].free) {
            struct vmr vmr = {
                .paddr = 0,
                .base  = NODE_ADDR(nodes[cur_node]),
                .size  = NODE_SIZE(nodes[cur_node]),
                .flags = VM_KRW,
            };

            vm_unmap(&vmr);
        }

        cur_node = nodes[cur_node].next;
    }

done:
    return;
}

void dump_nodes()
{
    printk("Nodes dump\n");
    unsigned i = 0;
    while (i < LAST_NODE_INDEX) {
        print_node(i);
        if(nodes[i].next == LAST_NODE_INDEX) break;
        i = nodes[i].next;
    }
}
