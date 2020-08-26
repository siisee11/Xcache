#ifndef PMNET_SERVER_H
#define PMNET_SERVER_H

#define LOG_SIZE (4294967296) // 40GB
#define INDEX_SIZE (536870912) // 10GB
/*
#define LOG_SIZE (42949672960) // 40GB
#define INDEX_SIZE (10737418240) // 10GB
*/
struct server_context{
    int node_id;
    void* local_ptr;
    int send_flags;
    int cur_node;
    int num_node;

    PMEMobjpool* log_pop;
    PMEMobjpool* index_pop;
    TOID(CCEH) hashtable;
};

#endif /* PMNET_SERVER_H */
