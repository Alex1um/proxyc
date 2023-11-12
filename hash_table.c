#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>

#define Gb * 1073741824
#define Mb * 1048576
#define Kb * 1024

typedef struct s_entry {
  char *key;
  char *value;
  size_t val_length;
  char is_ready;
} HashEntry;

typedef struct s_hash_table {
  HashEntry *entries;
  size_t size;
  pthread_mutex_t mutex;
  size_t get_attemts;
  size_t get_succeed;
  size_t available_space;
} HashTable;

size_t djb2_hash(unsigned char *str)
{
    unsigned long hash = 5381;
    int c;

    while (c = *str++)
        hash = ((hash << 5) + hash) + c; /* hash * 33 + c */
    return hash;
}

size_t simple_hash(char *key) {
  size_t i = 0;
  for (int j = 0; j < strlen(key); j++) i += key[j] * 31 * (j + 1);
  return i;
}

size_t jenkins_one_at_a_time_hash(char *key)
{
    size_t hash, i, len = strlen(key);
    for(hash = i = 0; i < len; ++i)
    {
        hash += key[i];
        hash += (hash << 10);
        hash ^= (hash >> 6);
    }
    hash += (hash << 3);
    hash ^= (hash >> 11);
    hash += (hash << 15);
    return hash;
}

size_t ht_hash(char *key) {
  return jenkins_one_at_a_time_hash(key);
}

void ht_monitor(HashTable *tb) {
  while (1) {
    if (pthread_mutex_trylock(&tb->mutex) == 0) {
      printf("stats: get %ld out of %ld; ratio: %f\n", tb->get_succeed, tb->get_attemts, (float)tb->get_succeed / (float)tb->get_attemts);
      pthread_mutex_unlock(&tb->mutex);
    }
    sleep(1);
  }
}

HashTable *ht_create(size_t size, size_t max_capacity_b) {
  HashTable *table = malloc(sizeof(HashTable));
  if (table == NULL) {
    return NULL;
  }

  if (pthread_mutex_init(&table->mutex, 0) != 0) {
    free(table);
    return NULL;
  }

  table->entries = malloc(sizeof(HashEntry) * size);
  if (table->entries == NULL) {
    free(table);
    pthread_mutex_destroy(&table->mutex);
    return NULL;
  }

  for (int i = 0; i < size; i++) {
    table->entries[i].key = NULL;
    table->entries[i].value = NULL;
    table->entries[i].val_length = 0;
    table->entries[i].is_ready = 0;
  }

  table->size = size;
  table->get_attemts = 0;
  table->get_succeed = 0;
  table->available_space = max_capacity_b;

  return table;
}

void ht_destroy(HashTable *table) {
  for (int i = 0; i < table->size; i++) {
    HashEntry e = table->entries[i];
    free(e.key);
    free(e.value);
  }
  pthread_mutex_destroy(&table->mutex);
  free(table->entries);
  free(table);
}

size_t ht_get(HashTable *table, char *key, char **out) {
  if (table == NULL || key == NULL) {
    return NULL;
  }
  pthread_mutex_lock(&table->mutex);

  size_t hash = ht_hash(key);
  size_t index = hash % table->size;

  HashEntry *entry = &table->entries[index];
  if (entry->key != NULL) {
    table->get_attemts++;
  }
  printf("get: %d - %d - %s == %s\n", hash, index, entry->key, key);
  if (entry->value != NULL && entry->is_ready == 1 && strcmp(entry->key, key) == 0) {
    // printf("found key: %s\n", key);
    table->get_succeed++;
    *out = entry->value;
    pthread_mutex_unlock(&table->mutex);
    return entry->val_length;
  }
  pthread_mutex_unlock(&table->mutex);
  return 0;
}

char *ht_realloc(char *ptr, size_t new_size) {
  // char *new_ptr = malloc(new_size);
  // strcpy(ptr, new_ptr);
  // free(ptr);
  // return new_ptr;
  return realloc(ptr, new_size);
}

void ht_cat(char *s1, size_t len1, char *s2, size_t len2) {
  for (size_t i = len1, j = 0; j < len2; j++, i++) {
    s1[i] = s2[j];
  }
}

void ht_cpy(char *dest, char *src, size_t src_len) {
  for (size_t i = 0; i < src_len; i++) {
    dest[i] = src[i];
  }
}

void ht_set_cancel(HashTable *table, char *key) {
  size_t hash = ht_hash(key);
  size_t index = hash % table->size;
  pthread_mutex_lock(&table->mutex);
  HashEntry *entry = &table->entries[index];
  if (entry != NULL && entry->key != NULL && entry->value != NULL) {
    entry->is_ready = 1;
  }
  pthread_mutex_unlock(&table->mutex);
}

void ht_set(HashTable *table, char *key, char *value, size_t new_len) {
  if (table == NULL || key == NULL || value == NULL) {
    return;
  }

  size_t hash = ht_hash(key);
  size_t index = hash % table->size;
  
  size_t lenv = new_len;
  size_t lenk = strlen(key);
  HashEntry *entry = &table->entries[index];

  pthread_mutex_lock(&table->mutex);
  size_t lene = (entry->value != NULL) ? entry->val_length : 0;
  size_t lenek = (entry->key != NULL) ? strlen(entry->key) : 0;
  
  if (entry->value != NULL && entry->key != NULL) {
    if (strcmp(entry->key, key) == 0) {
      if (lene < lenv && table->available_space >= lenv) {
        table->available_space += lene + 1;
        entry->value = ht_realloc(entry->value, sizeof(char) * (lenv + lene + 1));
        if (entry->value) {
          table->available_space -= (lene + lenv + 1);
          ht_cat(entry->value, lene, value, lenv);
          entry->val_length += lenv;
        } else {
          entry->key = NULL;
          entry->value = NULL;
        }
      } else {
        free(entry->value);
        free(entry->key);
        entry->val_length = 0;
        entry->value = NULL;
        entry->key = NULL;
      }
    } else if (table->available_space >= (lenv + lenk - lenek - lene)) {
      table->available_space += lene + lenek + 2;
      entry->value = ht_realloc(entry->value, sizeof(char) * (lenv + 1));
      entry->key = ht_realloc(entry->key, sizeof(char) * (lenk + 1));
      entry->is_ready = 0;
      if (entry->value && entry->key) {
        table->available_space -= (lenv + lenk + 2);
        ht_cpy(entry->value, value, lenv);
        strcpy(entry->key, key);
        entry->val_length = lenv;
      } else {
        entry->key = NULL;
        entry->value = NULL;
      }
    }
  } else if (table->available_space >= lenv + lenk + 2) {
    entry->value = malloc(sizeof(char) * (lenv + 1));
    entry->key = malloc(sizeof(char) * (lenk + 1));
    entry->is_ready = 0;
    if (entry->value != NULL && entry->key != NULL) {
      table->available_space -= (lenv + lenk + 2);
      ht_cpy(entry->value, value, lenv);
      strcpy(entry->key, key);
      entry->val_length = lenv;
    } else {
      entry->key = NULL;
      entry->value = NULL;
    }
  }
  pthread_mutex_unlock(&table->mutex);
}
