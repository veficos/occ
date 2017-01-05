

#ifndef __READER__H__
#define __READER__H__


#include "config.h"
#include "pmalloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


typedef enum reader_type_e {
    READER_TYPE_FILE,
    READER_TYPE_STRING,
} reader_type_t;


typedef struct file_reader_s* file_reader_t;
typedef struct string_reader_s* string_reader_t;


typedef struct reader_s {
    reader_type_t type;
    union {
        file_reader_t fr;
        string_reader_t sr;
    } u;
}* reader_t;


file_reader_t file_reader_create(const char *filename);
void file_reader_destroy(file_reader_t fr);
int file_reader_get(file_reader_t fr);
int file_reader_peek(file_reader_t fr);
void file_reader_unget(file_reader_t fr, char ch);
const char *file_reader_name(file_reader_t fr);


string_reader_t string_reader_create_n(const void *data, size_t n);
void string_reader_destroy(string_reader_t sr);
int string_reader_get(string_reader_t sr);
int string_reader_peek(string_reader_t sr);
void string_reader_unget(string_reader_t sr, int ch);
const char *string_reader_name(string_reader_t sr);


static inline
string_reader_t string_reader_create(const char *s)
{
    return string_reader_create_n(s, strlen(s));
}


static inline
reader_t reader_create(reader_type_t type, const char *s)
{
    reader_t reader = (reader_t) pmalloc(sizeof(struct reader_s));
    if (!reader) {
        return NULL;
    }

    reader->type = type;

    switch (type) {
    case READER_TYPE_STRING:
        reader->u.sr = string_reader_create(s);
        if (!reader->u.sr) {
            pfree(reader);
            return NULL;
        }
        return reader;
    case READER_TYPE_FILE:
        reader->u.fr = file_reader_create(s);
        if (!reader->u.fr) {
            pfree(reader);
            return NULL;
        }
        return reader;
    }

    return NULL;
}


static inline
void reader_destroy(reader_t reader)
{
    switch (reader->type) {
    case READER_TYPE_FILE:
        file_reader_destroy(reader->u.fr);
        break;
    case READER_TYPE_STRING:
        string_reader_destroy(reader->u.sr);
        break;
    }
    pfree(reader);
}


static inline
const char* reader_name(reader_t reader)
{
    switch (reader->type) {
    case READER_TYPE_FILE:
        return file_reader_name(reader->u.fr);
    case READER_TYPE_STRING:
        return string_reader_name(reader->u.sr);
    }
    return NULL;
}


static inline
int reader_get(reader_t reader)
{
    switch (reader->type) {
    case READER_TYPE_FILE:
        return file_reader_get(reader->u.fr);
    case READER_TYPE_STRING:
        return string_reader_get(reader->u.sr);
    }
    return EOF;
}


int reader_peek(reader_t reader)
{
    switch (reader->type) {
    case READER_TYPE_FILE:
        return file_reader_peek(reader->u.fr);
    case READER_TYPE_STRING:
        return string_reader_peek(reader->u.sr);
    }
    return EOF;
}


void reader_unget(reader_t reader, int ch)
{
    switch (reader->type) {
    case READER_TYPE_FILE:
        file_reader_unget(reader->u.fr, ch);
        break;
    case READER_TYPE_STRING:
        string_reader_unget(reader->u.sr, ch);
        break;
    }
}


#endif
