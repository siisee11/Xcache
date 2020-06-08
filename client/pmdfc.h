/*
 * pmdfc.h
 *
 * In kernel networking
 *
 * Copyright (c) 2019, Jaeyoun Nam, SKKU.
 */

#ifndef PMDFC_H
#define PMDFC_H


/* The pool holding the compressed pages */
extern struct page* page_pool;
extern wait_queue_head_t get_page_wait_queue;
extern int cond;

#endif
