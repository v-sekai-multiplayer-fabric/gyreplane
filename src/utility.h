#ifndef UTILITY_H_
#define UTILITY_H_

#include <h2o.h>
#include <stdint.h>
#include <stdbool.h>
#include <yajl/yajl_gen.h>

#include "global_data.h"

#define HELLO_RESPONSE "Hello, World!"

uint32_t get_random_number(uint32_t max, unsigned int *seed);

typedef struct {
    yajl_gen gen;
    bool in_use;
} json_generator_t;

json_generator_t *get_json_generator(json_generator_t **generators, size_t *count);
void free_json_generator(json_generator_t *gen, json_generator_t **generators, size_t *count, size_t max);
int send_json_gen(json_generator_t *gen, bool copy, h2o_req_t *req);
const char *get_query_param(const char *query, size_t query_len, const char *name, size_t name_len);

#endif
