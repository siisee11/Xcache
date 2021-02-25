#include <linux/module.h>
#include <linux/hashtable.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/atomic.h>


#define BITS 3 // 2^20 1M pages, 4KB x 1M = 4GB
//#define BITS 18 // 2^18 0.25M pages, 4KB x 0.25M = 1GB
#define NUM_PAGES (1 << BITS)

atomic_t rm_free_start;
uint64_t rm_free_end = PAGE_SIZE * NUM_PAGES; 

struct ht_data {
    uint64_t key;
    uint64_t roffset;
    struct hlist_node h_node;
};

DEFINE_HASHTABLE(hash_head, BITS);

static int __init init_ht_test(void)
{
    int i;
   
    hash_init(hash_head);

    pr_info("BITS: %d, NUM_PAGES: %lu\n", BITS, NUM_PAGES);    
    pr_info("strcut ht_data size: %d\n", sizeof(struct ht_data));

    for (i = 0; i < NUM_PAGES; i++) {
        struct ht_data *tmp = (struct ht_data *)kmalloc(sizeof(struct ht_data), GFP_ATOMIC);
        tmp->key = i;
        //tmp->roffset = i * PAGE_SIZE; //i << PAGE_SHIFT;
        tmp->roffset = atomic_fetch_add_unless(&rm_free_start, PAGE_SIZE, rm_free_end);
        if(tmp->roffset >= rm_free_end) {
            pr_err("Remote memory is full..");
            return 0;
        }

        hash_add(hash_head, &tmp->h_node, tmp->key);

        pr_info("for loop: key=%lld, roffset=%lld\n", tmp->key, tmp->roffset);
        /*
        if(i % 10000 == 0) {
            pr_info("for loop: key=%lld, roffset=%lld\n", tmp->key, tmp->roffset);
        }
        */
    }

    return 0; 
}
 
static void __exit exit_ht_test(void)
{
    int bkt;
    struct ht_data *cur;
    struct hlist_node *tmp_hnode;
    
    pr_info("exit_text: free data structure...\n");

    hash_for_each_safe(hash_head, bkt, tmp_hnode, cur, h_node) {
        hash_del(&cur->h_node);
        kfree(cur);
    }
    return;
}
 
module_init(init_ht_test);
module_exit(exit_ht_test);
 
MODULE_LICENSE("GPL");
