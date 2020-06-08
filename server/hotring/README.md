# HOTRING

## API

hotring_alloc()
hotring_insert()
hotring_get()
hotring_delete()
display()

## Example
struct hash_node *node, *prev;
struct hash *h = hotring_alloc(NBITS, KBITS);
hotring_insert(h, 30, item);
hotring_get(&h, 30, &node, &prev);
hotring_delete(h, 30);

## TODO
  - [x] ~~hotring_insert~~
  - [x] ~~hotring_get~~
  - [x] ~~hotring_delete~~
  - [x] ~~hotring_rehash~~
  - [x] ~~rehash condition~~
  - [ ] multi-threading

## PARAMETER
1. NBITS : number of bits for hash value
2. INCOME_THRESHOLD : When to rehash

## reference
hotring papaer (https://www.usenix.org/conference/fast20/presentation/chen-jiqiang)
