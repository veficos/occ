/**
 * 1. C11 5.1.1: "\r\n" or "\r" are canonicalized to "\n".
 *
 * 2. C11 5.1.1: Each instance of a backslash character (\) immediately
 *       followed by a new-line character is deleted, splicing physical
 *       source lines to form logical source lines.Only the last backslash
 *       on any physical source line shall be eligible for being part of
 *       such a splice. A source file that is not empty shall end in a
 *       new-line character, which shall not be immediately preceded by
 *       a backslash character before any such splicing takes place.
 *
 *     example:
 *         |#inc\
 *         |lude <stdio.h>
 *
 * 3. C11 5.1.1: EOF not immediately following a newline is converted to
 *       a sequence of newline and EOF. (The C spec requires source
 *       files end in a newline character (5.1.1.2p2). Thus, if all
 *       source files are conforming, this step wouldn't be needed.
 **/

#include "config.h"
#include "array.h"
#include "pmalloc.h"
#include "cstring.h"
#include "cspool.h"
#include "reader.h"
#include "utils.h"
#include "option.h"
#include "diagnostor.h"


#ifndef STREAM_STASHED_DEPTH
#define STREAM_STASHED_DEPTH     (12)
#endif


#ifndef READER_STREAM_DEPTH
#define READER_STREAM_DEPTH     (8)
#endif


struct stream_s {
    stream_type_t type;

    cstring_t fn;

    cstring_t stashed;

    linenote_t line_note;

    unsigned char *pc;
    unsigned char *pe;

    size_t line;
    size_t column;

    time_t modify_time;
    time_t change_time;
    time_t access_time;

    int lastch;
};


#define STREAM_STEP_BY_LINE(stream)         \
    do {                                    \
        (stream)->line++;                   \
        (stream)->column = 1;               \
        (stream)->line_note = (stream)->pc; \
    } while (false)


static bool __stream_init__(cspool_t *cspool, stream_t *stream,
                            stream_type_t type, const unsigned char *s);
static void __stream_uninit__(stream_t *stream);
static void __stream_push__(stream_t *stream, int ch);
static int __stream_pop__(stream_t *stream);
static int __stream_next__(stream_t *stream);
static int __stream_peek__(stream_t *stream);


reader_t* reader_create(void)
{
    reader_t *reader = (reader_t*) pmalloc(sizeof(reader_t));
    reader->cspool = cspool_create();
    reader->clean_csp = true;
    reader->streams = array_create_n(sizeof(stream_t), READER_STREAM_DEPTH);
    reader->last = NULL;
    return reader;
}


reader_t* reader_create_csp(cspool_t *csp)
{
    reader_t *reader = reader_create();
    reader->cspool = csp;
    reader->clean_csp = false;
    return reader;
}


void reader_destroy(reader_t *reader)
{
    stream_t *streams;
    size_t i;

    if (reader->clean_csp) {
        cspool_destroy(reader->cspool);
    }

    array_foreach(reader->streams, streams, i) {
        __stream_uninit__(&streams[i]);
    }

    array_destroy(reader->streams);

    pfree(reader);
}


size_t reader_depth(reader_t *reader)
{
    return array_length(reader->streams);
}


bool reader_is_empty(reader_t *reader)
{
    return array_length(reader->streams) == 0;
}


time_t reader_modify_time(reader_t *reader)
{
    assert(reader->last != NULL);
    return reader->last->modify_time;
}


time_t reader_change_time(reader_t *reader)
{
    assert(reader->last != NULL);
    return reader->last->change_time;
}


time_t reader_access_time(reader_t *reader)
{
    assert(reader->last != NULL);
    return reader->last->access_time;
}


bool reader_push(reader_t *reader, stream_type_t type, const unsigned char *s)
{
    stream_t *stream;

    stream = array_push_back(reader->streams);

    if (!__stream_init__(reader->cspool, stream, type, s)) {
        return false;
    }

    reader->last = stream;
    return true;
}


void reader_pop(reader_t *reader)
{
    assert(array_is_empty(reader->streams) == false);

    __stream_uninit__(reader->last);

    array_pop_back(reader->streams);

    if (array_is_empty(reader->streams)) {
        reader->last = NULL;
    } else {
        reader->last = &(array_cast_back(struct stream_s, reader->streams));
    }
}


int reader_get(reader_t *reader)
{
    if (reader->last != NULL) {
        return __stream_next__(reader->last);
    }
    return EOF;
}


int reader_peek(reader_t *reader)
{
    if (reader->last != NULL) {
        return __stream_peek__(reader->last);
    }
    return EOF;
}


void reader_unget(reader_t *reader, int ch)
{
    assert(ch != EOF && ch != '\0');
    __stream_push__(reader->last, ch);
}


bool reader_try(reader_t *reader, int ch)
{
    if (reader_peek(reader) == ch) {
        reader_get(reader);
        return true;
    }
    return false;
}


bool reader_test(reader_t *reader, int ch)
{
    return reader_peek(reader) == ch;
}


linenote_t reader_linenote(reader_t *reader)
{
    assert(reader->last != NULL);
    return reader->last->line_note;
}


size_t reader_line(reader_t *reader)
{
    assert(reader->last != NULL);
    return reader->last->line;
}


size_t reader_column(reader_t *reader)
{
    assert(reader->last != NULL);
    return reader->last->column;
}


cstring_t reader_filename(reader_t *reader)
{
    assert(reader->last != NULL);
    return reader->last->fn;
}


cstring_t linenote2cs(linenote_t linenote)
{
    const unsigned char *p = (const unsigned char *)linenote;

    while (*p) {
        if (*p == '\r' || *p == '\n') {
            break;
        }
        p++;
    }

    return cstring_new_n((const unsigned char*)linenote,
                         p - (const unsigned char *)linenote);
}


static
bool __stream_init__(cspool_t *cspool, stream_t *stream,
                     stream_type_t type, const unsigned char *s)
{
    cstring_t text = NULL;

    switch (type) {
    case STREAM_TYPE_FILE: {
        void *buf;
        FILE *fp;
        struct stat st;

        if ((fp = fopen(s, "rb")) == NULL) {
            return false;
        }

        if (fstat(fileno(fp), &st) != 0) {
            goto failure;
        }

        buf = pmalloc(st.st_size);
        if (fread(buf, sizeof(unsigned char), st.st_size, fp) != st.st_size) {
            goto failure;
        }
       
        stream->fn = cspool_push_cs(cspool, cstring_new(s));
        stream->modify_time = st.st_mtime;
        stream->access_time = st.st_atime;
        stream->change_time = st.st_ctime;
        text = cspool_push_cs(cspool, cstring_new_n(buf, st.st_size));

        pfree(buf);
        fclose(fp);
        break;
    failure:
        fclose(fp);
        return false;
    }
    case STREAM_TYPE_STRING: {
        stream->fn = cspool_push(cspool, "<string>");
        stream->modify_time = 0;
        stream->access_time = 0;
        stream->change_time = 0;
        text = cspool_push(cspool, s);
        break;
    }
    default:
        assert(false);
    }

    stream->type = type;
    stream->stashed = NULL;
    stream->line_note = stream->pc = text;
    stream->pe = &text[cstring_length(text)];
    stream->line = 1;
    stream->column = 1;
    stream->lastch = '\0';
    return true;
}


static
void __stream_uninit__(stream_t *stream)
{
    if (stream->stashed != NULL) {
        cstring_free(stream->stashed);
    }
}


static
void __stream_push__(stream_t *stream, int ch)
{
    if (stream->stashed == NULL) {
        stream->stashed = cstring_new_n(NULL, STREAM_STASHED_DEPTH);
    }
    stream->stashed = cstring_push_ch(stream->stashed, ch);
}


static
int __stream_pop__(stream_t *stream)
{
    return stream->stashed == NULL || cstring_length(stream->stashed) <= 0 ? \
        EOF : cstring_pop_ch(stream->stashed);
}


static
int __stream_next__(stream_t *stream)
{
    int ch;

    if ((ch = __stream_pop__(stream)) != EOF) {
        goto done;
    }

nextch:
    if (stream->pc >= stream->pe) {
        ch = stream->lastch == '\n' || 
            stream->lastch == EOF ? EOF : '\n';
        goto done;
    }

    ch = *stream->pc;

    stream->pc++;

    if (ch == '\r') {
        /**
         * "\r\n" or "\r" are canonicalized to "\n" 
         **/

        if (*stream->pc == '\n') {
            stream->pc++;
        }

        ch = '\n';
        STREAM_STEP_BY_LINE(stream);

    } else if (ch == '\n') {
        STREAM_STEP_BY_LINE(stream);

    } else if (ch == '\\') {
        /**
         * Each instance of a backslash character(\) immediately
         * followed by a newline character is deleted, splicing 
         * physical source lines to form logical source lines
         **/

        unsigned char *pc = stream->pc;
        uintptr_t step = 0;
        while (pc < stream->pe && ISSPACE(*pc)) {
            switch (*pc) {
            case '\r':
                if (*(pc + 1) == '\n') {
                    pc++;
                    step++;
                }
            case '\n':
                if (pc > stream->pc + step) {
                    if (option_get(w_backslash_newline_space)) {
                        warningf_with_linenote_position(stream->fn,
                                                        stream->line,
                                                        stream->column,
                                                        stream->line_note,
                                                        stream->column,
                                                        1,
                                                        "backslash and newline separated by space");
                    }
                }

                stream->pc = pc + 1;
                STREAM_STEP_BY_LINE(stream);
                goto nextch;
            }
            pc++;
        }

        if (pc == stream->pe) {
            if (option_get(warn_no_newline_eof)) {
                warningf_with_linenote_position(stream->fn,
                                                stream->line,
                                                stream->column,
                                                stream->line_note,
                                                stream->column,
                                                1,
                                                "backslash-newline at end of file");
            }

            ch = '\n';
            stream->pc = pc;
        }

    } else {
        stream->column++;
    }

done:
    stream->lastch = ch;
    return ch;
}


static int __stream_peek__(stream_t *stream)
{
    int ch;
    unsigned char *pc;

    if (stream->stashed != NULL &&
        cstring_length(stream->stashed) > 0) {
        return stream->stashed[cstring_length(stream->stashed) - 1];
    }

    pc = stream->pc;
nextch:
    if (pc >= stream->pe) {
        return stream->lastch == '\n' ||
            stream->lastch == EOF ? EOF : '\n';
    }

    ch = *pc;
    pc++;

    switch (ch) {
    case '\r':
    case '\n':
        return '\n';
    case '\\':
        while (pc < stream->pe && ISSPACE(*pc)) {
            switch (*pc) {
            case '\r':
                if (*(pc + 1) == '\n') {
                    pc++;
                }
            case '\n':
                pc++;
                goto nextch;
            }
            pc++;
        }
        if (pc == stream->pe) {
            return '\n';
        }
    }
    
    return ch;
}
