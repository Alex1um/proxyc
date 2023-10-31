#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>

typedef struct s_entry {
  char *key;
  char *value;
} HashEntry;

typedef struct s_hash_table {
  HashEntry *entries;
  size_t size;
  pthread_mutex_t mutex;
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
  for (int j = 0; j < strlen(key); j++) i += key[j] * 31 + ((j + 1) << 5);
  return i;
}

size_t jenkins_one_at_a_time_hash(char *key, size_t len)
{
    size_t hash, i;
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
  return simple_hash(key);
}

HashTable *ht_create(size_t size) {
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
  }

  table->size = size;

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

char *ht_get(HashTable *table, char *key) {
  if (table == NULL || key == NULL) {
    return NULL;
  }
  pthread_mutex_lock(&table->mutex);

  size_t hash = ht_hash(key);
  size_t index = hash % table->size;

  HashEntry entry = table->entries[index];
  printf("get: %d - %d - %s == %s\n", hash, index, entry.key, key);
  if (entry.value != NULL && strcmp(entry.key, key) == 0) {
    printf("found key: %s\n", key);
    pthread_mutex_unlock(&table->mutex);
    return entry.value;
  }
  pthread_mutex_unlock(&table->mutex);
  return NULL;
}

char *ht_realloc(char *ptr, size_t new_size) {
  // char *new_ptr = malloc(new_size);
  // strcpy(ptr, new_ptr);
  // free(ptr);
  // return new_ptr;
  return realloc(ptr, new_size);
}

void ht_set(HashTable *table, char *key, char *value) {
  if (table == NULL || key == NULL || value == NULL) {
    return;
  }

  size_t hash = ht_hash(key);
  size_t index = hash % table->size;
  
  pthread_mutex_lock(&table->mutex);
  HashEntry *entry = &table->entries[index];
  
  if (entry->value != NULL && entry->key != NULL) {
    if (strcmp(entry->key, key) == 0) {
      entry->value = ht_realloc(entry->value, sizeof(char) * (strlen(value) + 1 + strlen(entry->value) + 1));
      if (entry->value) {
        strcat(entry->value, value);
      } else {
        entry->key = NULL;
        entry->value = NULL;
      }
    } else {
      entry->value = ht_realloc(entry->value, sizeof(char) * (strlen(value) + 1));
      entry->key = ht_realloc(entry->key, sizeof(char) * (strlen(key) + 1));
      if (entry->value && entry->key) {
        strcpy(entry->value, value);
        strcpy(entry->key, key);
      } else {
        entry->key = NULL;
        entry->value = NULL;
      }
    }
  } else {
    entry->value = malloc(sizeof(char) * (strlen(value) + 1));
    entry->key = malloc(sizeof(char) * (strlen(key) + 1));
    if (entry->value != NULL && entry->key != NULL) {
      strcpy(entry->value, value);
      strcpy(entry->key, key);
    } else {
      entry->key = NULL;
      entry->value = NULL;
    }
  }
  pthread_mutex_unlock(&table->mutex);
}
