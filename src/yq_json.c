#include "yq_json.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>

/* JSON解析状态 */
typedef enum {
    JSON_STATE_INIT,
    JSON_STATE_VALUE,
    JSON_STATE_OBJECT_KEY,
    JSON_STATE_OBJECT_COLON,
    JSON_STATE_OBJECT_VALUE,
    JSON_STATE_ARRAY_ELEMENT,
    JSON_STATE_STRING,
    JSON_STATE_STRING_ESCAPE,
    JSON_STATE_NUMBER_INT,
    JSON_STATE_NUMBER_FRAC,
    JSON_STATE_NUMBER_EXP,
    JSON_STATE_TRUE,
    JSON_STATE_FALSE,
    JSON_STATE_NULL
} json_state_t;

/* JSON解析上下文 */
typedef struct {
    const char *input;
    size_t length;
    size_t pos;
    int depth;
    int max_depth;
    char *error;
} json_parser_t;

/* JSON错误消息 */
static const char *g_json_err_msgs[] = {
    "success",
    "JSON parse error",
    "out of memory",
    "JSON nesting depth exceeded",
    "invalid JSON format",
    "JSON data truncated",
    "unsupported JSON feature"
};

const char *yq_json_strerror(int rc) {
    if (rc < 0 || rc > 6) return "unknown JSON error";
    return g_json_err_msgs[rc];
}

/* 前向声明 */
static int json_parse_object(json_parser_t *parser, yq_json_value **out);
static int json_parse_array(json_parser_t *parser, yq_json_value **out);
static int json_parse_true(json_parser_t *parser, yq_json_value **out);
static int json_parse_false(json_parser_t *parser, yq_json_value **out);
static int json_parse_null(json_parser_t *parser, yq_json_value **out);

/* 创建JSON值 */
static yq_json_value *json_value_create(yq_json_type type) {
    yq_json_value *value = malloc(sizeof(yq_json_value));
    if (!value) return NULL;
    
    memset(value, 0, sizeof(yq_json_value));
    value->type = type;
    return value;
}

/* 错误处理 */
static void json_set_error(json_parser_t *parser, const char *msg) {
    if (parser->error) free(parser->error);
    parser->error = strdup(msg);
}

/* 跳过空白字符 */
static void json_skip_whitespace(json_parser_t *parser) {
    while (parser->pos < parser->length && 
           isspace((unsigned char)parser->input[parser->pos])) {
        parser->pos++;
    }
}

/* 解析字符串 */
static int json_parse_string(json_parser_t *parser, yq_json_value **out) {
    size_t start = parser->pos + 1;  // 跳过开始的引号
    size_t end = start;
    int escape = 0;
    
    while (end < parser->length) {
        if (escape) {
            escape = 0;
            end++;
        } else if (parser->input[end] == '\\') {
            escape = 1;
            end++;
        } else if (parser->input[end] == '"') {
            break;
        } else {
            end++;
        }
    }
    
    if (end >= parser->length) {
        json_set_error(parser, "unterminated string");
        return YQ_JSON_ERR;
    }
    
    size_t str_len = end - start;
    char *str_data = malloc(str_len + 1);
    if (!str_data) {
        return YQ_JSON_NOMEM;
    }
    
    memcpy(str_data, parser->input + start, str_len);
    str_data[str_len] = '\0';
    
    *out = json_value_create(YQ_JSON_STRING);
    if (!*out) {
        free(str_data);
        return YQ_JSON_NOMEM;
    }
    
    (*out)->data.string.data = str_data;
    (*out)->data.string.length = str_len;
    parser->pos = end + 1;  // 跳过结束的引号
    
    return YQ_JSON_OK;
}

/* 解析数字 */
static int json_parse_number(json_parser_t *parser, yq_json_value **out) {
    size_t start = parser->pos;
    size_t end = start;
    
    // 解析整数部分
    if (parser->input[start] == '-') {
        end++;
    }
    
    while (end < parser->length && isdigit((unsigned char)parser->input[end])) {
        end++;
    }
    
    // 解析小数部分
    if (end < parser->length && parser->input[end] == '.') {
        end++;
        while (end < parser->length && isdigit((unsigned char)parser->input[end])) {
            end++;
        }
    }
    
    // 解析指数部分
    if (end < parser->length && (parser->input[end] == 'e' || parser->input[end] == 'E')) {
        end++;
        if (end < parser->length && (parser->input[end] == '+' || parser->input[end] == '-')) {
            end++;
        }
        while (end < parser->length && isdigit((unsigned char)parser->input[end])) {
            end++;
        }
    }
    
    char *num_str = malloc(end - start + 1);
    if (!num_str) {
        return YQ_JSON_NOMEM;
    }
    
    memcpy(num_str, parser->input + start, end - start);
    num_str[end - start] = '\0';
    
    double num = strtod(num_str, NULL);
    free(num_str);
    
    *out = json_value_create(YQ_JSON_NUMBER);
    if (!*out) {
        return YQ_JSON_NOMEM;
    }
    
    (*out)->data.number = num;
    parser->pos = end;
    
    return YQ_JSON_OK;
}

/* 解析值 */
static int json_parse_value(json_parser_t *parser, yq_json_value **out) {
    json_skip_whitespace(parser);
    
    if (parser->pos >= parser->length) {
        json_set_error(parser, "unexpected end of input");
        return YQ_JSON_ERR;
    }
    
    char c = parser->input[parser->pos];
    
    switch (c) {
        case '{':
            parser->pos++;
            return json_parse_object(parser, out);
        case '[':
            parser->pos++;
            return json_parse_array(parser, out);
        case '"':
            return json_parse_string(parser, out);
        case 't':
            return json_parse_true(parser, out);
        case 'f':
            return json_parse_false(parser, out);
        case 'n':
            return json_parse_null(parser, out);
        case '-':
        case '0':
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7':
        case '8':
        case '9':
            return json_parse_number(parser, out);
        default:
            json_set_error(parser, "unexpected character");
            return YQ_JSON_ERR;
    }
}

/* 解析对象 */
static int json_parse_object(json_parser_t *parser, yq_json_value **out) {
    json_skip_whitespace(parser);
    
    *out = json_value_create(YQ_JSON_OBJECT);
    if (!*out) {
        return YQ_JSON_NOMEM;
    }
    
    if (parser->input[parser->pos] == '}') {
        parser->pos++;
        return YQ_JSON_OK;
    }
    
    parser->depth++;
    if (parser->depth > parser->max_depth) {
        json_set_error(parser, "JSON nesting depth exceeded");
        return YQ_JSON_DEPTH;
    }
    
    while (1) {
        json_skip_whitespace(parser);
        
        if (parser->input[parser->pos] != '"') {
            json_set_error(parser, "expected string key");
            return YQ_JSON_ERR;
        }
        
        yq_json_value *key_value = NULL;
        int rc = json_parse_string(parser, &key_value);
        if (rc != YQ_JSON_OK) {
            return rc;
        }
        
        json_skip_whitespace(parser);
        if (parser->input[parser->pos] != ':') {
            json_set_error(parser, "expected colon after key");
            yq_json_value_destroy(key_value);
            return YQ_JSON_ERR;
        }
        parser->pos++;
        
        yq_json_value *value = NULL;
        rc = json_parse_value(parser, &value);
        if (rc != YQ_JSON_OK) {
            yq_json_value_destroy(key_value);
            return rc;
        }
        
        rc = yq_json_object_add(*out, key_value->data.string.data, value);
        yq_json_value_destroy(key_value);
        if (rc != YQ_JSON_OK) {
            yq_json_value_destroy(value);
            return rc;
        }
        
        json_skip_whitespace(parser);
        if (parser->input[parser->pos] == '}') {
            parser->pos++;
            parser->depth--;
            return YQ_JSON_OK;
        }
        
        if (parser->input[parser->pos] != ',') {
            json_set_error(parser, "expected comma or closing brace");
            return YQ_JSON_ERR;
        }
        parser->pos++;
    }
}

/* 解析数组 */
static int json_parse_array(json_parser_t *parser, yq_json_value **out) {
    json_skip_whitespace(parser);
    
    *out = json_value_create(YQ_JSON_ARRAY);
    if (!*out) {
        return YQ_JSON_NOMEM;
    }
    
    if (parser->input[parser->pos] == ']') {
        parser->pos++;
        return YQ_JSON_OK;
    }
    
    parser->depth++;
    if (parser->depth > parser->max_depth) {
        json_set_error(parser, "JSON nesting depth exceeded");
        return YQ_JSON_DEPTH;
    }
    
    while (1) {
        json_skip_whitespace(parser);
        
        yq_json_value *value = NULL;
        int rc = json_parse_value(parser, &value);
        if (rc != YQ_JSON_OK) {
            return rc;
        }
        
        rc = yq_json_array_add(*out, value);
        if (rc != YQ_JSON_OK) {
            yq_json_value_destroy(value);
            return rc;
        }
        
        json_skip_whitespace(parser);
        if (parser->input[parser->pos] == ']') {
            parser->pos++;
            parser->depth--;
            return YQ_JSON_OK;
        }
        
        if (parser->input[parser->pos] != ',') {
            json_set_error(parser, "expected comma or closing bracket");
            return YQ_JSON_ERR;
        }
        parser->pos++;
    }
}

/* 解析true */
static int json_parse_true(json_parser_t *parser, yq_json_value **out) {
    if (parser->pos + 3 < parser->length &&
        strncmp(parser->input + parser->pos, "true", 4) == 0) {
        *out = json_value_create(YQ_JSON_BOOL);
        if (!*out) {
            return YQ_JSON_NOMEM;
        }
        (*out)->data.boolean = 1;
        parser->pos += 4;
        return YQ_JSON_OK;
    }
    
    json_set_error(parser, "invalid true literal");
    return YQ_JSON_ERR;
}

/* 解析false */
static int json_parse_false(json_parser_t *parser, yq_json_value **out) {
    if (parser->pos + 4 < parser->length &&
        strncmp(parser->input + parser->pos, "false", 5) == 0) {
        *out = json_value_create(YQ_JSON_BOOL);
        if (!*out) {
            return YQ_JSON_NOMEM;
        }
        (*out)->data.boolean = 0;
        parser->pos += 5;
        return YQ_JSON_OK;
    }
    
    json_set_error(parser, "invalid false literal");
    return YQ_JSON_ERR;
}

/* 解析null */
static int json_parse_null(json_parser_t *parser, yq_json_value **out) {
    if (parser->pos + 3 < parser->length &&
        strncmp(parser->input + parser->pos, "null", 4) == 0) {
        *out = json_value_create(YQ_JSON_NULL);
        if (!*out) {
            return YQ_JSON_NOMEM;
        }
        parser->pos += 4;
        return YQ_JSON_OK;
    }
    
    json_set_error(parser, "invalid null literal");
    return YQ_JSON_ERR;
}

/* 主解析函数 */
int yq_json_parse(const char *json_str, size_t length, yq_json_value **out) {
    if (!json_str || !out) {
        return YQ_JSON_ERR;
    }
    
    if (length == 0) {
        length = strlen(json_str);
    }
    
    json_parser_t parser;
    parser.input = json_str;
    parser.length = length;
    parser.pos = 0;
    parser.depth = 0;
    parser.max_depth = 128;  // 默认最大深度
    parser.error = NULL;
    
    int rc = json_parse_value(&parser, out);
    
    if (parser.error) {
        free(parser.error);
    }
    
    if (rc != YQ_JSON_OK || parser.pos < parser.length) {
        if (*out) {
            yq_json_value_destroy(*out);
            *out = NULL;
        }
        return rc;
    }
    
    return YQ_JSON_OK;
}

/* JSON生成辅助函数 */
static int json_value_generate(const yq_json_value *value, int pretty, char **out, int indent) {
    if (!value || !out) {
        return YQ_JSON_ERR;
    }
    
    switch (value->type) {
        case YQ_JSON_NULL:
            *out = strdup("null");
            break;
            
        case YQ_JSON_BOOL:
            *out = value->data.boolean ? strdup("true") : strdup("false");
            break;
            
        case YQ_JSON_NUMBER: {
            char buffer[64];
            snprintf(buffer, sizeof(buffer), "%g", value->data.number);
            *out = strdup(buffer);
            break;
        }
            
        case YQ_JSON_STRING: {
            size_t len = value->data.string.length;
            size_t escaped_len = len + 2;  // 加上引号
            char *escaped = malloc(escaped_len + 1);
            if (!escaped) return YQ_JSON_NOMEM;
            
            escaped[0] = '"';
            memcpy(escaped + 1, value->data.string.data, len);
            escaped[len + 1] = '\0';
            *out = escaped;
            break;
        }
            
        case YQ_JSON_ARRAY: {
            size_t count = value->data.array.count;
            if (count == 0) {
                *out = strdup("[]");
                return YQ_JSON_OK;
            }
            
            size_t total_len = 2;  // [ ]
            char **elements = malloc(count * sizeof(char *));
            if (!elements) return YQ_JSON_NOMEM;
            
            for (size_t i = 0; i < count; i++) {
                char *element = NULL;
                int rc = json_value_generate(&value->data.array.elements[i], pretty, &element, indent + 1);
                if (rc != YQ_JSON_OK) {
                    for (size_t j = 0; j < i; j++) {
                        free(elements[j]);
                    }
                    free(elements);
                    return rc;
                }
                elements[i] = element;
                total_len += strlen(element);
                if (i < count - 1) {
                    total_len += 2;  // , 和可能的空格
                }
            }
            
            if (pretty) {
                total_len += count * (indent + 1) + 1;  // 换行和缩进
            }
            
            char *result = malloc(total_len + 1);
            if (!result) {
                for (size_t i = 0; i < count; i++) {
                    free(elements[i]);
                }
                free(elements);
                return YQ_JSON_NOMEM;
            }
            
            size_t pos = 0;
            result[pos++] = '[';
            if (pretty) {
                result[pos++] = '\n';
            }
            
            for (size_t i = 0; i < count; i++) {
                if (pretty) {
                    for (int j = 0; j < indent + 1; j++) {
                        result[pos++] = ' ';
                    }
                }
                size_t len = strlen(elements[i]);
                memcpy(result + pos, elements[i], len);
                pos += len;
                free(elements[i]);
                
                if (i < count - 1) {
                    result[pos++] = ',';
                    if (pretty) {
                        result[pos++] = '\n';
                    }
                }
            }
            
            free(elements);
            
            if (pretty) {
                result[pos++] = '\n';
                for (int j = 0; j < indent; j++) {
                    result[pos++] = ' ';
                }
            }
            result[pos++] = ']';
            result[pos] = '\0';
            *out = result;
            break;
        }
        
        case YQ_JSON_OBJECT: {
            size_t count = value->data.object.count;
            if (count == 0) {
                *out = strdup("{}");
                return YQ_JSON_OK;
            }
            
            size_t total_len = 2;  // { }
            char **elements = malloc(count * sizeof(char *));
            if (!elements) return YQ_JSON_NOMEM;
            
            for (size_t i = 0; i < count; i++) {
                char *key_part = malloc(strlen(value->data.object.members[i].key) + 3);
                if (!key_part) {
                    for (size_t j = 0; j < i; j++) {
                        free(elements[j]);
                    }
                    free(elements);
                    return YQ_JSON_NOMEM;
                }
                sprintf(key_part, "\"%s\":", value->data.object.members[i].key);
                
                char *value_part = NULL;
                int rc = json_value_generate(value->data.object.members[i].value, pretty, &value_part, indent + 1);
                if (rc != YQ_JSON_OK) {
                    free(key_part);
                    for (size_t j = 0; j < i; j++) {
                        free(elements[j]);
                    }
                    free(elements);
                    return rc;
                }
                
                size_t key_len = strlen(key_part);
                size_t value_len = strlen(value_part);
                elements[i] = malloc(key_len + value_len + 2);  // +2 for space and comma
                if (!elements[i]) {
                    free(key_part);
                    free(value_part);
                    for (size_t j = 0; j < i; j++) {
                        free(elements[j]);
                    }
                    free(elements);
                    return YQ_JSON_NOMEM;
                }
                
                memcpy(elements[i], key_part, key_len);
                elements[i][key_len] = ' ';
                memcpy(elements[i] + key_len + 1, value_part, value_len);
                elements[i][key_len + 1 + value_len] = '\0';
                
                free(key_part);
                free(value_part);
                
                total_len += key_len + value_len + 2;  // key + space + value + comma
                if (i < count - 1) {
                    total_len += 2;  // comma and space
                }
            }
            
            if (pretty) {
                total_len += count * (indent + 1) + 1;  // 换行和缩进
            }
            
            char *result = malloc(total_len + 1);
            if (!result) {
                for (size_t i = 0; i < count; i++) {
                    free(elements[i]);
                }
                free(elements);
                return YQ_JSON_NOMEM;
            }
            
            size_t pos = 0;
            result[pos++] = '{';
            if (pretty) {
                result[pos++] = '\n';
            }
            
            for (size_t i = 0; i < count; i++) {
                if (pretty) {
                    for (int j = 0; j < indent + 1; j++) {
                        result[pos++] = ' ';
                    }
                }
                size_t len = strlen(elements[i]);
                memcpy(result + pos, elements[i], len);
                pos += len;
                free(elements[i]);
                
                if (i < count - 1) {
                    result[pos++] = ',';
                    if (pretty) {
                        result[pos++] = '\n';
                    }
                }
            }
            
            free(elements);
            
            if (pretty) {
                result[pos++] = '\n';
                for (int j = 0; j < indent; j++) {
                    result[pos++] = ' ';
                }
            }
            result[pos++] = '}';
            result[pos] = '\0';
            *out = result;
            break;
        }
        
        default:
            return YQ_JSON_ERR;
    }
    
    if (!*out) {
        return YQ_JSON_NOMEM;
    }
    
    return YQ_JSON_OK;
}

int yq_json_generate(const yq_json_value *value, int pretty, char **out) {
    return json_value_generate(value, pretty, out, 0);
}

/* 释放JSON值 */
void yq_json_value_destroy(yq_json_value *value) {
    if (!value) return;
    
    switch (value->type) {
        case YQ_JSON_STRING:
            free(value->data.string.data);
            break;
            
        case YQ_JSON_ARRAY: {
            for (size_t i = 0; i < value->data.array.count; i++) {
                yq_json_value_destroy(&value->data.array.elements[i]);
            }
            free(value->data.array.elements);
            break;
        }
        
        case YQ_JSON_OBJECT: {
            for (size_t i = 0; i < value->data.object.count; i++) {
                free(value->data.object.members[i].key);
                yq_json_value_destroy(value->data.object.members[i].value);
            }
            free(value->data.object.members);
            break;
        }
        
        default:
            break;
    }
    
    free(value);
}

/* 创建JSON值 */
yq_json_value *yq_json_create_null(void) {
    return json_value_create(YQ_JSON_NULL);
}

yq_json_value *yq_json_create_bool(int boolean) {
    yq_json_value *value = json_value_create(YQ_JSON_BOOL);
    if (value) {
        value->data.boolean = boolean ? 1 : 0;
    }
    return value;
}

yq_json_value *yq_json_create_number(double number) {
    yq_json_value *value = json_value_create(YQ_JSON_NUMBER);
    if (value) {
        value->data.number = number;
    }
    return value;
}

yq_json_value *yq_json_create_string(const char *str, size_t length) {
    if (!str) return NULL;
    
    if (length == 0) {
        length = strlen(str);
    }
    
    yq_json_value *value = json_value_create(YQ_JSON_STRING);
    if (!value) return NULL;
    
    value->data.string.data = malloc(length + 1);
    if (!value->data.string.data) {
        free(value);
        return NULL;
    }
    
    memcpy(value->data.string.data, str, length);
    value->data.string.data[length] = '\0';
    value->data.string.length = length;
    
    return value;
}

yq_json_value *yq_json_create_array(void) {
    return json_value_create(YQ_JSON_ARRAY);
}

yq_json_value *yq_json_create_object(void) {
    return json_value_create(YQ_JSON_OBJECT);
}

/* 向数组添加元素 */
int yq_json_array_add(yq_json_value *array, yq_json_value *value) {
    if (!array || !value || array->type != YQ_JSON_ARRAY) {
        return YQ_JSON_ERR;
    }
    
    size_t new_count = array->data.array.count + 1;
    yq_json_value **new_elements = realloc(array->data.array.elements, new_count * sizeof(yq_json_value *));
    if (!new_elements) {
        return YQ_JSON_NOMEM;
    }
    
    array->data.array.elements = new_elements;
    array->data.array.elements[array->data.array.count] = *value;
    array->data.array.count = new_count;
    
    return YQ_JSON_OK;
}

/* 向对象添加成员 */
int yq_json_object_add(yq_json_value *object, const char *key, yq_json_value *value) {
    if (!object || !key || !value || object->type != YQ_JSON_OBJECT) {
        return YQ_JSON_ERR;
    }
    
    size_t new_count = object->data.object.count + 1;
    struct {
        char *key;
        yq_json_value *value;
    } *new_members = realloc(object->data.object.members, new_count * sizeof(*new_members));
    if (!new_members) {
        return YQ_JSON_NOMEM;
    }
    
    object->data.object.members = new_members;
    
    char *key_copy = strdup(key);
    if (!key_copy) {
        return YQ_JSON_NOMEM;
    }
    
    object->data.object.members[object->data.object.count].key = key_copy;
    object->data.object.members[object->data.object.count].value = value;
    object->data.object.count = new_count;
    
    return YQ_JSON_OK;
}

/* 获取类型字符串 */
const char *yq_json_type_to_string(yq_json_type type) {
    switch (type) {
        case YQ_JSON_NULL:   return "null";
        case YQ_JSON_BOOL:   return "bool";
        case YQ_JSON_NUMBER: return "number";
        case YQ_JSON_STRING: return "string";
        case YQ_JSON_ARRAY:  return "array";
        case YQ_JSON_OBJECT: return "object";
        default:              return "unknown";
    }
}

/* yq-DB集成函数 */
int yq_json_put(yq_txn *txn, yq_slice key, const yq_json_value *json_value) {
    if (!txn || !key.data || !json_value) {
        return YQ_ERR_INVAL;
    }
    
    char *json_str = NULL;
    int rc = yq_json_generate(json_value, 0, &json_str);
    if (rc != YQ_JSON_OK) {
        return YQ_ERR_INVAL;
    }
    
    yq_slice val = { json_str, strlen(json_str) };
    rc = yq_put(txn, key, val, YQ_PUT_UPSERT);
    free(json_str);
    
    return rc;
}

int yq_json_get(yq_txn *txn, yq_slice key, yq_json_value **out) {
    if (!txn || !key.data || !out) {
        return YQ_ERR_INVAL;
    }
    
    yq_slice json_str = { 0 };
    int rc = yq_get(txn, key, &json_str);
    if (rc != YQ_OK) {
        return rc;
    }
    
    rc = yq_json_parse(json_str.data, json_str.size, out);
    return rc;
}

int yq_json_put_raw(yq_txn *txn, yq_slice key, const char *json_str) {
    if (!txn || !key.data || !json_str) {
        return YQ_ERR_INVAL;
    }
    
    yq_slice val = { json_str, strlen(json_str) };
    return yq_put(txn, key, val, YQ_PUT_UPSERT);
}

int yq_json_get_raw(yq_txn *txn, yq_slice key, yq_slice *out) {
    if (!txn || !key.data || !out) {
        return YQ_ERR_INVAL;
    }
    
    return yq_get(txn, key, out);
}