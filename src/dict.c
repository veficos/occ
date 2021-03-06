/** Hash Tables Implementation.
 *
 * This file implements in memory hash tables with insert/del/replace/find/
 * get-random-element operations. Hash tables will auto resize if needed
 * tables of power of two in size are used, collisions are handled by
 * chaining. See the source code for more information... :)
 *
 * Copyright (c) 2006-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 **/


#include "config.h"
#include "pmalloc.h"
#include "dict.h"
#include "hash.h"


static inline bool __dict_expand_if_needed__(dict_t *ht);
static inline unsigned long __dict_next_power__(unsigned long size);
static inline int __dict_key_index__(dict_t *d, const void *key, unsigned int hash, dict_entry_t **existing);
static inline bool __dict_init__(dict_t *ht, dict_type_t *type, void *ud);


static unsigned int dict_force_resize_ratio = 5;
static uint8_t dict_hash_function_seed[16];


void dict_set_hash_function_seed(uint8_t *seed) {
    memcpy(dict_hash_function_seed, seed, sizeof(dict_hash_function_seed));
}


uint8_t* dict_get_hash_function_seed(void) {
    return dict_hash_function_seed;
}


uint64_t dict_gen_hash_function(const void *key, int len) {
    return siphash(key, len, dict_hash_function_seed);
}


uint64_t dict_gen_case_hash_function(const unsigned char *buf, int len) {
    return siphash_nocase(buf, len, dict_hash_function_seed);
}


/**
 * Reset a hash table already initialized with ht_init().
 * NOTE: This function should only be called by ht_destroy().
 **/
static inline
void __dict_reset__(dict_hash_table_t *ht)
{
    ht->table = NULL;
    ht->size = 0;
    ht->mask = 0;
    ht->used = 0;
}


dict_t* dict_create(dict_type_t *type, void *ud)
{
    dict_t* d = pmalloc(sizeof(dict_t));

    __dict_init__(d, type, ud);

    return d;
}


bool __dict_init__(dict_t *d, dict_type_t *type, void *ud)
{
    __dict_reset__(&d->ht[0]);
    __dict_reset__(&d->ht[1]);

    d->type      = type;
    d->ud        = ud;
    d->rehashidx = -1;
    d->iterators = 0;

    return true;
}


/**
 * Resize the table to the minimal size that contains all the elements,
 * but with the invariant of a USED/BUCKETS ratio near to <= 1
 **/
bool dict_resize(dict_t *d)
{
    int minimal;

    if (!d->dict_can_resize || dict_is_rehashing(d)) {
        return false;
    }

    minimal = d->ht[0].used;

    if (minimal < DICT_HASH_TABLE_INITIAL_SIZE) {
        minimal = DICT_HASH_TABLE_INITIAL_SIZE;
    }

    return dict_expand(d, minimal);
}


/**
 * Expand or create the hash table
 **/
bool dict_expand(dict_t *d, unsigned long size)
{
    dict_hash_table_t n;
    unsigned long realsize;


    realsize = __dict_next_power__(size);

    /**
     * the size is invalid if it is smaller than the number of
     * elements already inside the hash table
     **/
    if (dict_is_rehashing(d) || d->ht[0].used > size)
        return false;

    /* Rehashing to the same table size is not useful. */
    if (realsize == d->ht[0].size) {
        return false;
    }

    /* Allocate the new hash table and initialize all pointers to NULL */
    n.size  = realsize;
    n.mask  = realsize - 1;
    n.table = pcalloc(1, realsize * sizeof(dict_entry_t*));
    n.used  = 0;

    /**
     * Is this the first initialization? If so it's not really a rehashing
     * we just set the first hash table so that it can accept keys.
     **/
    if (d->ht[0].table == NULL) {
        d->ht[0] = n;
        return true;
    }

    /* Prepare a second hash table for incremental rehashing */
    d->ht[1] = n;
    d->rehashidx = 0;
    return true;
}


/**
 * Performs N steps of incremental rehashing. Returns true if there are still
 * keys to move from the old to the new hash table, otherwise false is returned.
 *
 * Note that a rehashing step consists in moving a bucket (that may have more
 * than one key as we use chaining) from the old to the new hash table, however
 * since part of the hash table may be composed of empty spaces, it is not
 * guaranteed that this function will rehash even a single bucket, since it
 * will visit at max N*10 empty buckets in total, otherwise the amount of
 * work it does would be unbound and the function may block for a long time.
 **/
bool dict_rehash(dict_t *d, int n) {
    /* Max number of empty buckets to visit. */
    int empty_visits = n * 10;

    if (!dict_is_rehashing(d)) {
        return false;
    }

    while (n-- && d->ht[0].used != 0) {
        dict_entry_t *de, *nextde;

        assert(d->ht[0].size > (unsigned long)d->rehashidx);

        while (d->ht[0].table[d->rehashidx] == NULL) {
            d->rehashidx++;
            if (--empty_visits == 0) return 1;
        }

        de = d->ht[0].table[d->rehashidx];

        while (de) {
            unsigned int h;

            nextde = de->next;

            h = dict_hash_key(d, de->key) & d->ht[1].mask;

            de->next = d->ht[1].table[h];
            d->ht[1].table[h] = de;
            d->ht[0].used--;
            d->ht[1].used++;
            de = nextde;
        }

        d->ht[0].table[d->rehashidx] = NULL;
        d->rehashidx++;
    }

    if (d->ht[0].used == 0) {
        pfree(d->ht[0].table);
        d->ht[0] = d->ht[1];
        __dict_reset__(&d->ht[1]);
        d->rehashidx = -1;
        return false;
    }

    /* More to rehash... */
    return true;
}


/**
 * This function performs just a step of rehashing, and only if there are
 * no safe iterators bound to our hash table. When we have iterators in the
 * middle of a rehashing we can't mess with the two hash tables otherwise
 * some element can be missed or duplicated.
 *
 * This function is called by common lookup or update operations in the
 * dictionary so that the hash table automatically migrates from H1 to H2
 * while it is actively used.
 **/
static void __dict_rehash_step__(dict_t *d) {
    if (d->iterators == 0) {
        dict_rehash(d, 1);
    }
}


/**
 * Add an element to the target hash table
 **/
bool dict_add(dict_t *d, void *key, void *val)
{
    dict_entry_t *entry = dict_add_raw(d, key, NULL);

    if (!entry) {
        return false;
    }

    dict_set_val(d, entry, val);

    return true;
}


/**
 * Low level add or find:
 * This function adds the entry but instead of setting a value returns the
 * dictEntry structure to the user, that will make sure to fill the value
 * field as he wishes.
 *
 * This function is also directly exposed to the user API to be called
 * mainly in order to store non-pointers inside the hash value, example:
 *
 * entry = dictAddRaw(dict,mykey,NULL);
 * if (entry != NULL) dictSetSignedIntegerVal(entry,1000);
 *
 * Return values:
 *
 * If key already exists NULL is returned, and "*existing" is populated
 * with the existing entry if existing is not NULL.
 *
 * If key was added, the hash entry is returned to be manipulated by the caller.
 **/
dict_entry_t* dict_add_raw(dict_t *d, void *key, dict_entry_t **existing)
{
    int index;
    dict_entry_t *entry;
    dict_hash_table_t *ht;

    if (dict_is_rehashing(d)) {
        __dict_rehash_step__(d);
    }

    /**
     * Get the index of the new element, or -1 if
     * the element already exists.
     **/
    if ((index = __dict_key_index__(d, key, (unsigned int) dict_hash_key(d, key), existing)) == -1) {
        return NULL;
    }

    /** Allocate the memory and store the new entry.
     * Insert the element in top, with the assumption that in a database
     * system it is more likely that recently added entries are accessed
     * more frequently.
     **/
    ht = dict_is_rehashing(d) ? &d->ht[1] : &d->ht[0];
    entry = pmalloc(sizeof(*entry));
    entry->next = ht->table[index];
    ht->table[index] = entry;
    ht->used++;

    /* Set the hash entry fields. */
    dict_set_key(d, entry, key);

    return entry;
}


/**
 * Add or Overwrite:
 * Add an element, discarding the old value if the key already exists.
 * Return true if the key was added from scratch, false if there was already an
 * element with such key and dict_replace() just performed a value update
 * operation.
 **/
bool dict_replace(dict_t *d, void *key, void *val)
{
    dict_entry_t *entry, *existing, auxentry;

    /**
     * Try to add the element. If the key
     * does not exists dictAdd will suceed.
     **/
    entry = dict_add_raw(d, key, &existing);
    if (entry) {
        dict_set_val(d, entry, val);
        return true;
    }

    /**
     * Set the new value and free the old one. Note that it is important
     * to do that in this order, as the value may just be exactly the same
     * as the previous one. In this context, think to reference counting,
     * you want to increment (set), and then decrement (free), and not the
     * reverse.
     **/
    auxentry = *existing;
    dict_set_val(d, existing, val);
    dict_free_val(d, &auxentry);
    return false;
}


/**
 * Add or Find:
 * dictAddOrFind() is simply a version of dictAddRaw() that always
 * returns the hash entry of the specified key, even if the key already
 * exists and can't be added (in that case the entry of the already
 * existing key is returned.)
 *
 * See dictAddRaw() for more information.
 **/
dict_entry_t* dict_add_or_find(dict_t *d, void *key)
{
    dict_entry_t *entry, *existing;

    entry = dict_add_raw(d, key, &existing);

    return entry ? entry : existing;
}


/**
 * Search and remove an element. This is an helper function for
 * dict_delete() and dict_unlink(), please check the top comment
 * of those functions.
 **/
static dict_entry_t* __dict_generic_delete__(dict_t *d, const void *key, bool nofree)
{
    unsigned int h, idx;
    dict_entry_t *he, *prevhe;
    int table;

    if (d->ht[0].used == 0 && d->ht[1].used == 0) {
        return NULL;
    }

    if (dict_is_rehashing(d)) {
        __dict_rehash_step__(d);
    }

    h = (unsigned int) dict_hash_key(d, key);

    for (table = 0; table <= 1; table++) {
        idx = h & d->ht[table].mask;
        he = d->ht[table].table[idx];
        prevhe = NULL;

        while (he) {
            if (key == he->key || dict_compare_keys(d, key, he->key)) {
                /* Unlink the element from the list */
                if (prevhe) {
                    prevhe->next = he->next;
                } else {
                    d->ht[table].table[idx] = he->next;
                }

                if (!nofree) {
                    dict_free_key(d, he);
                    dict_free_val(d, he);
                    pfree(he);
                }

                d->ht[table].used--;
                return he;
            }
            prevhe = he;
            he = he->next;
        }

        if (!dict_is_rehashing(d)) {
            break;
        }
    }

    return NULL;
}


/**
 * Remove an element, returning DICT_OK on success or DICT_ERR if the
 * element was not found.
 */
bool dict_delete(dict_t *ht, const void *key) {
    return __dict_generic_delete__(ht, key, false) ? true : false;
}


/**
 * Remove an element from the table, but without actually releasing
 * the key, value and dictionary entry. The dictionary entry is returned
 * if the element was found (and unlinked from the table), and the user
 * should later call `dictFreeUnlinkedEntry()` with it in order to release it.
 * Otherwise if the key is not found, NULL is returned.
 *
 * This function is useful when we want to remove something from the hash
 * table but want to use its value before actually deleting the entry.
 * Without this function the pattern would require two lookups:
 *
 *  entry = dictFind(...);
 *  // Do something with entry
 *  dictDelete(dictionary,entry);
 *
 * Thanks to this function it is possible to avoid this, and use
 * instead:
 *
 * entry = dictUnlink(dictionary,entry);
 * // Do something with entry
 * dictFreeUnlinkedEntry(entry); // <- This does not need to lookup again.
 **/
dict_entry_t* dict_unlink(dict_t *ht, const void *key)
{
    return __dict_generic_delete__(ht, key, true);
}


/**
 * You need to call this function to really free the entry after a call
 * to dictUnlink(). It's safe to call this function with 'he' = NULL.
 **/
void dict_free_unlinked_entry(dict_t *d, dict_entry_t *he)
{
    if (he == NULL) {
        return;
    }

    dict_free_key(d, he);
    dict_free_val(d, he);
    pfree(he);
}


static
bool __dict_clear__(dict_t *d, dict_hash_table_t *ht, void(*callback)(void *))
{
    unsigned long i;

    for (i = 0; i < ht->size && ht->used > 0; i++) {
        dict_entry_t *he, *next;

        if (callback && (i & 65535) == 0) {
            callback(d->ud);
        }

        if ((he = ht->table[i]) == NULL) {
            continue;
        }

        while (he) {
            next = he->next;
            dict_free_key(d, he);
            dict_free_val(d, he);
            pfree(he);
            ht->used--;
            he = next;
        }
    }

    pfree(ht->table);
    __dict_reset__(ht);
    return true;
}


void dict_destroy(dict_t *d)
{
    __dict_clear__(d, &d->ht[0], NULL);
    __dict_clear__(d, &d->ht[1], NULL);
    pfree(d);
}


dict_entry_t* dict_find(dict_t *d, const void *key)
{
    dict_entry_t *he;
    unsigned int h, idx, table;

    if (d->ht[0].used + d->ht[1].used == 0) {
        return NULL;
    }

    if (dict_is_rehashing(d)) {
        __dict_rehash_step__(d);
    }

    h = (unsigned int) dict_hash_key(d, key);
    for (table = 0; table <= 1; table++) {
        idx = h & d->ht[table].mask;
        he = d->ht[table].table[idx];
        while (he) {
            if (key == he->key || dict_compare_keys(d, key, he->key)) {
                return he;
            }
            he = he->next;
        }

        if (!dict_is_rehashing(d)) {
            return NULL;
        }
    }

    return NULL;
}


void *dict_fetch_value(dict_t *d, const void *key) {
    dict_entry_t *he;

    he = dict_find(d, key);

    return he ? dict_get_val(he) : NULL;
}


/**
 * A fingerprint is a 64 bit number that represents the state of the dictionary
 * at a given time, it's just a few dict properties xored together.
 * When an unsafe iterator is initialized, we get the dict fingerprint, and check
 * the fingerprint again when the iterator is released.
 * If the two fingerprints are different it means that the user of the iterator
 * performed forbidden operations against the dictionary while iterating.
 **/
long long dict_finger_print(dict_t *d) {
    long long integers[6], hash = 0;
    int j;

    integers[0] = (long)d->ht[0].table;
    integers[1] = d->ht[0].size;
    integers[2] = d->ht[0].used;
    integers[3] = (long)d->ht[1].table;
    integers[4] = d->ht[1].size;
    integers[5] = d->ht[1].used;

    /**
     * We hash N integers by summing every successive integer with the integer
     * hashing of the previous sum. Basically:
     *
     * Result = hash(hash(hash(int1)+int2)+int3) ...
     *
     * This way the same set of integers in a different order will (likely) hash
     * to a different number.
     **/
    for (j = 0; j < 6; j++) {
        hash += integers[j];

        /* For the hashing step we use Tomas Wang's 64 bit integer hash. */
        hash = (~hash) + (hash << 21);
        hash = hash ^ (hash >> 24);
        hash = (hash + (hash << 3)) + (hash << 8);
        hash = hash ^ (hash >> 14);
        hash = (hash + (hash << 2)) + (hash << 4);
        hash = hash ^ (hash >> 28);
        hash = hash + (hash << 31);
    }

    return hash;
}


dict_iterator_t* dict_get_iterator(dict_t *d)
{
    dict_iterator_t* iter = pmalloc(sizeof(*iter));

    iter->d = d;
    iter->table = 0;
    iter->index = -1;
    iter->safe = false;
    iter->entry = NULL;
    iter->next = NULL;
    return iter;
}


dict_iterator_t* dict_get_safe_iterator(dict_t *d) {
    dict_iterator_t* i = dict_get_iterator(d);
    i->safe = true;
    return i;
}


dict_entry_t* dict_next(dict_iterator_t *iter)
{
    for(;;) {
        if (iter->entry == NULL) {
            dict_hash_table_t *ht = &iter->d->ht[iter->table];
            if (iter->index == -1 && iter->table == 0) {
                if (iter->safe) {
                    iter->d->iterators++;
                } else {
                    iter->fingerprint = dict_finger_print(iter->d);
                }
            }

            iter->index++;
            if (iter->index >= (long)ht->size) {
                if (dict_is_rehashing(iter->d) && iter->table == 0) {
                    iter->table++;
                    iter->index = 0;
                    ht = &iter->d->ht[1];
                } else {
                    break;
                }
            }

            iter->entry = ht->table[iter->index];
        } else {
            iter->entry = iter->next;
        }

        if (iter->entry) {
            /**
             * We need to save the 'next' here, the iterator user
             * may delete the entry we are returning.
             **/
            iter->next = iter->entry->next;
            return iter->entry;
        }
    }

    return NULL;
}


void dict_release_iterator(dict_iterator_t *iter)
{
    if (!(iter->index == -1 && iter->table == 0)) {
        if (iter->safe) {
            iter->d->iterators--;
        } else {
            assert(iter->fingerprint == dict_finger_print(iter->d));
        }
    }

    pfree(iter);
}


/**
 * Function to reverse bits. Algorithm from:
 * http://graphics.stanford.edu/~seander/bithacks.html#ReverseParallel
 **/
static inline 
unsigned long __reverse_parallel__(unsigned long v) {
    /* bit size; must be power of 2 */
    unsigned long s = 8 * sizeof(v);
    unsigned long mask = ~0;
    while ((s >>= 1) > 0) {
        mask ^= (mask << s);
        v = ((v >> s) & mask) | ((v << s) & ~mask);
    }
    return v;
}

/**
 * dict_scan() is used to iterate over the elements of a dictionary.
 *
 * Iterating works the following way:
 *
 * 1) Initially you call the function using a cursor (v) value of 0.
 * 2) The function performs one step of the iteration, and returns the
 *    new cursor value you must use in the next call.
 * 3) When the returned cursor is 0, the iteration is complete.
 *
 * The function guarantees all elements present in the
 * dictionary get returned between the start and end of the iteration.
 * However it is possible some elements get returned multiple times.
 *
 * For every element returned, the callback argument 'fn' is
 * called with 'privdata' as first argument and the dictionary entry
 * 'de' as second argument.
 *
 * HOW IT WORKS.
 *
 * The iteration algorithm was designed by Pieter Noordhuis.
 * The main idea is to increment a cursor starting from the higher order
 * bits. That is, instead of incrementing the cursor normally, the bits
 * of the cursor are reversed, then the cursor is incremented, and finally
 * the bits are reversed again.
 *
 * This strategy is needed because the hash table may be resized between
 * iteration calls.
 *
 * dict.c hash tables are always power of two in size, and they
 * use chaining, so the position of an element in a given table is given
 * by computing the bitwise AND between Hash(key) and SIZE-1
 * (where SIZE-1 is always the mask that is equivalent to taking the rest
 *  of the division between the Hash of the key and SIZE).
 *
 * For example if the current hash table size is 16, the mask is
 * (in binary) 1111. The position of a key in the hash table will always be
 * the last four bits of the hash output, and so forth.
 *
 * WHAT HAPPENS IF THE TABLE CHANGES IN SIZE?
 *
 * If the hash table grows, elements can go anywhere in one multiple of
 * the old bucket: for example let's say we already iterated with
 * a 4 bit cursor 1100 (the mask is 1111 because hash table size = 16).
 *
 * If the hash table will be resized to 64 elements, then the new mask will
 * be 111111. The new buckets you obtain by substituting in ??1100
 * with either 0 or 1 can be targeted only by keys we already visited
 * when scanning the bucket 1100 in the smaller hash table.
 *
 * By iterating the higher bits first, because of the inverted counter, the
 * cursor does not need to restart if the table size gets bigger. It will
 * continue iterating using cursors without '1100' at the end, and also
 * without any other combination of the final 4 bits already explored.
 *
 * Similarly when the table size shrinks over time, for example going from
 * 16 to 8, if a combination of the lower three bits (the mask for size 8
 * is 111) were already completely explored, it would not be visited again
 * because we are sure we tried, for example, both 0111 and 1111 (all the
 * variations of the higher bit) so we don't need to test it again.
 *
 * WAIT... YOU HAVE *TWO* TABLES DU RING REHASHING!
 *
 * Yes, this is true, but we always iterate the smaller table first, then
 * we test all the expansions of the current cursor into the larger
 * table. For example if the current cursor is 101 and we also have a
 * larger table of size 16, we also test (0)101 and (1)101 inside the larger
 * table. This reduces the problem back to having only one table, where
 * the larger one, if it exists, is just an expansion of the smaller one.
 *
 * LIMITATIONS
 *
 * This iterator is completely stateless, and this is a huge advantage,
 * including no additional memory used.
 *
 * The disadvantages resulting from this design are:
 *
 * 1) It is possible we return elements more than once. However this is usually
 *    easy to deal with in the application level.
 * 2) The iterator must return multiple elements per call, as it needs to always
 *    return all the keys chained in a given bucket, and all the expansions, so
 *    we are sure we don't miss keys moving during rehashing.
 * 3) The reverse cursor is somewhat hard to understand at first, but this
 *    comment is supposed to help.
 **/
unsigned long dict_scan(dict_t *d, unsigned long v, dict_scan_function_pt scan_fn,
                        dict_scan_bucket_function_pt bucket_fn, void *ud)
{
    dict_hash_table_t *t0, *t1;
    dict_entry_t *de, *next;
    unsigned long m0, m1;

    if (dict_is_empty(d)) {
        return 0;
    }

    if (!dict_is_rehashing(d)) {
        t0 = &(d->ht[0]);
        m0 = t0->mask;

        /* Emit entries at cursor */
        if (bucket_fn) {
            bucket_fn(ud, &t0->table[v & m0]);
        }

        de = t0->table[v & m0];
        while (de) {
            next = de->next;
            scan_fn(ud, de);
            de = next;
        }

    } else {
        t0 = &d->ht[0];
        t1 = &d->ht[1];

        /* Make sure t0 is the smaller and t1 is the bigger table */
        if (t0->size > t1->size) {
            t0 = &d->ht[1];
            t1 = &d->ht[0];
        }

        m0 = t0->mask;
        m1 = t1->mask;

        /* Emit entries at cursor */
        if (bucket_fn) {
            bucket_fn(ud, &t0->table[v & m0]);
        }

        de = t0->table[v & m0];
        while (de) {
            next = de->next;
            scan_fn(ud, de);
            de = next;
        }

        /**
         * Iterate over indices in larger table that are the expansion
         * of the index pointed to by the cursor in the smaller table
         **/
        do {
            /* Emit entries at cursor */
            if (bucket_fn) {
                bucket_fn(ud, &t1->table[v & m1]);
            }

            de = t1->table[v & m1];
            while (de) {
                next = de->next;
                scan_fn(ud, de);
                de = next;
            }

            /* Increment bits not covered by the smaller mask */
            v = (((v | m0) + 1) & ~m0) | (v & m0);

            /* Continue while bits covered by mask difference is non-zero */
        } while (v & (m0 ^ m1));
    }

    /**
     * Set unmasked bits so incrementing the reversed cursor
     * operates on the masked bits of the smaller table
     **/
    v |= ~m0;

    /* Increment the reverse cursor */
    v = __reverse_parallel__(v);
    v++;
    v = __reverse_parallel__(v);

    return v;
}


static inline
bool __dict_expand_if_needed__(dict_t *d)
{
    /* Incremental rehashing already in progress. Return. */
    if (dict_is_rehashing(d)) {
        return true;
    }

    /* If the hash table is empty expand it to the initial size. */
    if (d->ht[0].size == 0) {
        return dict_expand(d, DICT_HASH_TABLE_INITIAL_SIZE);
    }

    /**
     * If we reached the 1:1 ratio, and we are allowed to resize the hash
     * table (global setting) or we should avoid it but the ratio between
     * elements/buckets is over the "safe" threshold, we resize doubling
     * the number of buckets.
     **/
    if (d->ht[0].used >= d->ht[0].size &&
        (d->dict_can_resize ||
            d->ht[0].used / d->ht[0].size > dict_force_resize_ratio)) {
        return dict_expand(d, d->ht[0].used * 2);
    }

    return true;
}


/* Our hash table capability is a power of two */
static inline
unsigned long __dict_next_power__(unsigned long size)
{
    unsigned long i = DICT_HASH_TABLE_INITIAL_SIZE;

    if (size >= LONG_MAX) return LONG_MAX;
    while (1) {
        if (i >= size) {
            return i;
        }
        i *= 2;
    }
}


/**
 * Returns the index of a free slot that can be populated with
 * a hash entry for the given 'key'.
 * If the key already exists, -1 is returned
 * and the optional output parameter may be filled.
 *
 * Note that if we are in the process of rehashing the hash table, the
 * index is always returned in the context of the second (new) hash table.
 **/
static int __dict_key_index__(dict_t *d, const void *key, unsigned int hash, dict_entry_t **existing)
{
    unsigned int idx, table;
    dict_entry_t *he;

    if (existing) {
        *existing = NULL;
    }

    /* Expand the hash table if needed */
    if (__dict_expand_if_needed__(d) == false) {
        return -1;
    }

    for (table = 0; table <= 1; table++) {
        idx = hash & d->ht[table].mask;
        /* Search if this slot does not already contain the given key */
        he = d->ht[table].table[idx];
        while (he) {
            if (key == he->key || dict_compare_keys(d, key, he->key)) {
                if (existing) {
                    *existing = he;
                }
                return -1;
            }
            he = he->next;
        }

        if (!dict_is_rehashing(d)) {
            break;
        }
    }

    return idx;
}


void dict_empty(dict_t *d, void(*callback)(void*)) {
    __dict_clear__(d, &d->ht[0], callback);
    __dict_clear__(d, &d->ht[1], callback);

    d->rehashidx = -1;
    d->iterators = 0;
}


void dict_enable_resize(dict_t *d) {
    d->dict_can_resize = true;
}


void dict_disable_resize(dict_t *d) {
    d->dict_can_resize = false;
}


unsigned int dict_get_hash(dict_t *d, const void *key) {
    return (unsigned int) dict_hash_key(d, key);
}


/**
 * Finds the dict_entry_t reference by using pointer and pre-calculated hash.
 * oldkey is a dead pointer and should not be accessed.
 * the hash value should be provided using dictGetHash.
 * no string / key comparison is performed.
 * return value is the reference to the dict_entry_t if found, or NULL if not found.
 **/
dict_entry_t** dict_find_entry_ref_by_ptr_and_hash(dict_t *d, const void *oldptr, unsigned int hash) {
    dict_entry_t *he, **heref;
    unsigned int idx, table;

    if (d->ht[0].used + d->ht[1].used == 0) {
        return NULL;
    }

    for (table = 0; table <= 1; table++) {
        idx = hash & d->ht[table].mask;
        heref = &d->ht[table].table[idx];
        he = *heref;

        while (he) {
            if (oldptr == he->key) {
                return heref;
            }
            heref = &he->next;
            he = *heref;
        }

        if (!dict_is_rehashing(d)) {
            return NULL;
        }
    }

    return NULL;
}


#ifdef COLLECT_DICT_STATS


static
void __init_dict_hash_table_stat__(dict_hash_table_stat_t *ht_stat)
{
    int i;
    ht_stat->table_size = 0;
    ht_stat->number_of_elements = 0;
    ht_stat->different_slots = 0;
    ht_stat->max_chain_length = 0;
    ht_stat->counted_avg_chain_length = 0;
    ht_stat->computed_avg_chain_length = 0;
    for (i = 0; i < DICT_STATS_VECTLEN; i++) {
        ht_stat->clvector[i] = 0;
    }
}


void __dict_get_stats_ht__(dict_hash_table_t *ht, dict_hash_table_stat_t *ht_stat) {
    unsigned long i, slots = 0, chainlen, maxchainlen = 0;
    unsigned long totchainlen = 0;
    size_t l = 0;

    if (ht->used == 0) {
        return;
    }

    for (i = 0; i < ht->size; i++) {
        dict_entry_t *he;

        if (ht->table[i] == NULL) {
            ht_stat->clvector[0]++;
            continue;
        }

        slots++;

        /* For each hash entry on this slot... */
        chainlen = 0;
        he = ht->table[i];
        while (he) {
            chainlen++;
            he = he->next;
        }

        ht_stat->clvector[(chainlen < DICT_STATS_VECTLEN) ? chainlen : (DICT_STATS_VECTLEN - 1)]++;
        if (chainlen > maxchainlen) maxchainlen = chainlen;
        totchainlen += chainlen;
    }

    /* Generate human readable stats. */
    ht_stat->table_size = ht->size;
    ht_stat->number_of_elements = ht->used;
    ht_stat->different_slots = slots;
    ht_stat->max_chain_length = maxchainlen;
    ht_stat->counted_avg_chain_length = (double)totchainlen / (double)slots;
    ht_stat->computed_avg_chain_length = (double)ht->used / (double)slots;
}


void dict_get_stats(dict_t *d, dict_stat_t* stats)
{
    __init_dict_hash_table_stat__(&stats->main);
    __init_dict_hash_table_stat__(&stats->rehashing);

    __dict_get_stats_ht__(&d->ht[0], &stats->main);

    if (dict_is_rehashing(d)) {
        __dict_get_stats_ht__(&d->ht[1], &stats->rehashing);
    }
}


#endif
