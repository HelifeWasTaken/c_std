#include "json.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#define _JSON_CREATE_ALLOCATOR_CHECKED(var, allocator_code, code, ret) \
{ \
    (var) = allocator_code; \
    if ((var) == NULL) { \
        _set_last_error(JSON_ERROR_OUT_OF_MEMORY, "Out of Memory"); \
        { code }; \
        return (ret); \
    } \
}

#define JSON_CREATE_CHECKED(var, type, code, ret) \
    _JSON_CREATE_ALLOCATOR_CHECKED(var, json_create((type)), code, ret)

#define JSON_CREATE_VECTOR_CHECKED(var, code, ret) \
    _JSON_CREATE_ALLOCATOR_CHECKED(var, vector_create(sizeof(JsonElement)), code, ret)

#define JSON_CREATE_MAP_CHECKED(var, code, ret) \
    _JSON_CREATE_ALLOCATOR_CHECKED(var, map_create(compare_strings_json, string_deallocator_json, json_element_deallocator), code, ret)

#define JSON_CREATE_STRDUP_CHECKED(var, str, code, ret) \
    _JSON_CREATE_ALLOCATOR_CHECKED(var, strdup(str), code, ret)

#define JSON_CREATE_STRNDUP_CHECKED(var, str, n, code, ret) \
    _JSON_CREATE_ALLOCATOR_CHECKED(var, strndup(str, n), code, ret)

#define JSON_CREATE_DEEP_COPY_CHECKED(var, element, code, ret) \
    _JSON_CREATE_ALLOCATOR_CHECKED(var, json_deep_copy(element), code, ret)

#define JSON_CREATE_VECTOR_PUSH_BACK_CHECKED(vec, element, code, ret) \
{ \
    if (!vector_push_back(vec, element)) { \
        _set_last_error(JSON_ERROR_OUT_OF_MEMORY, "Out of Memory"); \
        { code }; \
        return ret; \
    } \
}

#define JSON_CREATE_MAP_INSERT_CHECKED(map, key, value, code, ret) \
{ \
    if (!map_insert(map, key, value)) { \
        _set_last_error(JSON_ERROR_OUT_OF_MEMORY, "Out of Memory or duplicate key"); \
        { code }; \
        return ret; \
    } \
}

#define JSON_PARSE_VALUE_CHECKED(state, value, code, except, ret) \
{ \
    if (_parse_value(state, value) != except) { \
        { code }; \
        return ret; \
    } \
}

#define JSON_CREATE_TYPE_CHECK(var, _type, code, ret) \
{ \
    if ((var)->type == (_type)) { \
        _set_last_error(JSON_ERROR_INVALID_TYPE, "Invalid type"); \
        { code }; \
        return ret; \
    } \
}

#define _JSON_SERIALIZE_APPEND_STR(_str, _append, _len, _size) \
{ \
    if ((_str = _append_str(_str, _append, _len, _size)) == NULL) { \
        return NULL; \
    } \
}

#define _JSON_SERIALIZE_APPEND_IDENT(_str, _ident, _depth, _len, _size) \
{ \
    if ((_str = _ident_append(_str, _ident, _depth, _len, _size)) == NULL) { \
        return NULL; \
    } \
}

#define _JSON_SERIALIZE_R_CHECKED(_element, _ident, _depth, _str, _len, _size) \
{ \
    if ((_str = json_serialize_r(_element, _ident, _depth, _str, _len, _size)) == NULL) { \
        return NULL; \
    } \
}

static JsonError last_error = {0, ""};
// static Mutex json_error_mutex = MUTEX_INITIALIZER;
static const JsonParseOptions json_default_parse_options = {
    .strict = false,
    .allow_comments = false,
    .allow_trailing_comma = false,
    .allow_number_extensions = false,
    .allow_single_quoted_strings = false,
    .allow_unquoted_keys = false,
    .allow_string_c_style_escapes = true,
    .allow_utf8 = false
};

static void _set_last_error(JsonErrorCode code, const char *message)
{
    // c_std_mutex_lock(&json_error_mutex);
    last_error.code = code;
    strncpy(last_error.message, message, 256);
    fmt_fprintf(stderr, "Error: %s\n", message);
    // c_std_mutex_unlock(&json_error_mutex);
}

JsonError json_last_error()
{
    JsonError error;
    // c_std_mutex_lock(&json_error_mutex);
    error = last_error;
    // c_std_mutex_unlock(&json_error_mutex);
    return error;
}

static void _get_line_and_column(const char *input, size_t position, size_t *line, size_t *column)
{
    *line = 1;
    *column = 1;
    for (size_t i = 0; i < position; i++) {
        if (input[i] == '\n') {
            (*line)++;
            *column = 1;
        } else {
            (*column)++;
        }
    }
}

static void _set_error_at_position(JsonParserState *state, JsonErrorCode code, const char *const message)
{
    size_t line, column;
    _get_line_and_column(state->input, state->position, &line, &column);
    char error_message[256];
    snprintf(error_message, 256, "%s at %zu:%zu", message, line, column);
    _set_last_error(code, error_message);
}

static bool _is_position_at_end(JsonParserState *state)
{
    return state->position >= state->input_len;
}

static char _get_char_at_position(JsonParserState *state)
{
    return _is_position_at_end(state) ? '\0' : state->input[state->position];
}

static bool _is_char_at_position(JsonParserState *state, char c)
{
    return !_is_position_at_end(state) && _get_char_at_position(state) == c;
}

static bool _is_token_at_position(JsonParserState *state, const char *token)
{
    size_t token_len = strlen(token);
    if (state->position + token_len > state->input_len) {
        return false;
    }
    return strncmp(state->input + state->position, token, token_len) == 0;
}

static bool _all_is_str(const char *str, int (*predicate)(int))
{
    size_t len = strlen(str);
    for (size_t i = 0; i < len; i++) {
        if (!predicate(str[i])) {
            return false;
        }
    }
    return true;
}

static void json_element_deallocator(void* data) {
    if (!data) 
        return;
    JsonElement* element = (JsonElement*)data;
    json_deallocate(element);
}

static int compare_strings_json(const KeyType a, const KeyType b) {
    return strcmp((const char*)a, (const char*)b);
}

static void string_deallocator_json(void *data) {
    free(data);
} 

// Json utils

JsonElement *json_create(JsonType type)
{
    JsonElement *element;
    _JSON_CREATE_ALLOCATOR_CHECKED(element, malloc(sizeof(JsonElement)), {}, NULL);

    element->type = type;
    switch (type) {
        case JSON_NULL:
            break;
        case JSON_BOOL:
            element->value.bool_val = false;
            break;
        case JSON_STRING:
            element->value.string_val = NULL;
            break;
        case JSON_NUMBER:
            element->value.number_val = 0.0;
            break;
        case JSON_ARRAY:
            JSON_CREATE_VECTOR_CHECKED(element->value.array_val, { free(element); }, NULL);
            break;
        case JSON_OBJECT:
            JSON_CREATE_MAP_CHECKED(element->value.object_val, { free(element); }, NULL);
            break;
    }
    return element;
}

void json_deallocate(JsonElement *element)
{
    if (element == NULL) {
        return;
    }

    switch (element->type) {
        case JSON_NULL:
        case JSON_BOOL:
        case JSON_NUMBER:
            break;
        case JSON_STRING:
            free(element->value.string_val);
            break;
        case JSON_ARRAY:
            VECTOR_FOREACH(JsonElement *, element->value.array_val, it, {
                json_deallocate(*it);
            });
            vector_deallocate(element->value.array_val);
            break;
        case JSON_OBJECT:
            map_deallocate(element->value.object_val);
            break;
    }
    free(element);
}

JsonElement* json_deep_copy(const JsonElement *element)
{
    JsonElement *copy;
    JSON_CREATE_CHECKED(copy, element ? element->type : JSON_NULL, {}, NULL);

    switch (element->type) {
        case JSON_NULL:
            break;
        case JSON_BOOL:
            copy->value.bool_val = element->value.bool_val;
            break;
        case JSON_STRING:
            JSON_CREATE_STRDUP_CHECKED(copy->value.string_val, element->value.string_val, { json_deallocate(copy); }, NULL);
            break;
        case JSON_NUMBER:
            copy->value.number_val = element->value.number_val;
            break;
        case JSON_ARRAY: {
            VECTOR_FOREACH(JsonElement *, element->value.array_val, it, {
                JsonElement *item;
                JSON_CREATE_DEEP_COPY_CHECKED(item, *it, { json_deallocate(copy); }, NULL);
                JSON_CREATE_VECTOR_PUSH_BACK_CHECKED(copy->value.array_val, &item, { json_deallocate(copy); }, NULL);
            });

            break;
        }

        case JSON_OBJECT: {
            MAP_FOREACH(char *, kvalue, JsonElement *, pvalue, element->value.object_val, {
                char *key;
                JsonElement *value;
                JSON_CREATE_STRDUP_CHECKED(key, kvalue, { json_deallocate(copy); }, NULL);
                JSON_CREATE_DEEP_COPY_CHECKED(value, pvalue, { free(key); json_deallocate(copy); }, NULL);
                JSON_CREATE_MAP_INSERT_CHECKED(copy->value.object_val, key, value, { free(key); json_deallocate(copy); }, NULL);
            });

            break;
        }
    }
}

JsonType json_typeof(const JsonElement *element)
{
    return element->type;
}

size_t json_object_size(const JsonElement *object)
{
    return map_size(object->value.object_val);
}

size_t json_array_size(const JsonElement *array)
{
    return vector_size(array->value.array_val);
}

// every parser consider that the first character is valid and non whitespace except for _parse_whitespace

static JsonParserStateResult _parse_whitespace(JsonParserState *state)
{
    while (state->position < state->input_len && isspace(state->input[state->position])) {
        state->position++;
    }
    return JSON_PARSER_STATE_CONTINUE;
}

static JsonParserStateResult _parse_comment(JsonParserState *state)
{
    _parse_whitespace(state);
    if (_is_token_at_position(state, "/*")) {
        while (state->position < state->input_len) {
            if (_is_token_at_position(state, "*/")) {
                state->position += 2;
                return _parse_comment(state); // parse next comment and remove the whitespace
            }
            state->position++;
        }
        // unfinished comment
        if (state->position >= state->input_len) {
            _set_error_at_position(state, JSON_ERROR_UNFINISHED_COMMENT, "Unfinished comment");
            return JSON_PARSER_STATE_ERROR;
        }
    } else if (_is_token_at_position(state, "//")) {
        while (_get_char_at_position(state) != '\n') {
            state->position++;
        }
        return _parse_comment(state); // parse next comment and remove the whitespace
    }
    return JSON_PARSER_STATE_CONTINUE;
}

static JsonParserStateResult _parse_null(JsonParserState *state, JsonElement *element)
{
    if (_is_token_at_position(state, "null")) {
        element->type = JSON_NULL;
        state->position += 4;
        return JSON_PARSER_STATE_OK;
    }
    return JSON_PARSER_STATE_CONTINUE;
}

static JsonParserStateResult _parse_boolean(JsonParserState *state, JsonElement *element)
{
    if (_is_token_at_position(state, "true")) {
        element->type = JSON_BOOL;
        element->value.bool_val = true;
        state->position += 4;
        return JSON_PARSER_STATE_OK;
    } else if (_is_token_at_position(state, "false")) {
        element->type = JSON_BOOL;
        element->value.bool_val = false;
        state->position += 5;
        return JSON_PARSER_STATE_OK;
    }
    return JSON_PARSER_STATE_CONTINUE;
}

static JsonParserStateResult _parse_string(JsonParserState *state, JsonElement *element)
{
    static const char escapes[] = { 'b', 'f', 'n', 'r', 't', '\\', '/', '"' };
    static const char escaped[] = { '\b', '\f', '\n', '\r', '\t', '\\', '/', '"' };
    size_t start, end, escape_count;
    char *string;

    if (!_is_char_at_position(state, '"')) {
        return JSON_PARSER_STATE_CONTINUE;
    }

    start = ++state->position;

    // find the end of the string while considering escape sequences
    escape_count = 0;
    while (state->position < state->input_len) {
        if (_is_char_at_position(state, '"')) {
            break;
        } else if (_is_char_at_position(state, '\\')) {
            state->position++;
            if (state->position >= state->input_len) {
                _set_error_at_position(state, JSON_ERROR_UNFINISHED_STRING, "Unfinished string");
                return JSON_PARSER_STATE_ERROR;
            }
            escape_count++;
        }
        state->position++;
    }

    // if we reached the end of the string without finding a closing quote
    if (state->position >= state->input_len) {
        _set_error_at_position(state, JSON_ERROR_UNFINISHED_STRING, "Unfinished string");
        return JSON_PARSER_STATE_ERROR;
    }

    if (escape_count >= 1 && state->options.allow_string_c_style_escapes == false) {
        _set_error_at_position(state, JSON_ERROR_NON_ENABLED_EXTENSION, "C-style escape sequences are not enabled");
        return JSON_PARSER_STATE_ERROR;
    }

    // allocate a string of the correct size
    end = state->position++;
    _JSON_CREATE_ALLOCATOR_CHECKED(string, malloc(end - start + 1 - escape_count + 1),
                                {}, JSON_PARSER_STATE_ERROR);

    size_t string_index = 0;
    while (start < end) {

        // copy the raw char if it's not an escape sequence
        if (state->input[start] != '\\') {
            string[string_index++] = state->input[start++];
            continue;
        }

        // get the escape sequence
        bool found = false;
        for (size_t i = 0; i < sizeof(escapes); i++) {
            if (state->input[start] == escapes[i]) {
                string[string_index++] = escaped[i];
                found = true;
                break;
            }
        }

        // Building escape sequence
        if (!found) {
            _set_error_at_position(state, JSON_ERROR_UNIMPLEMENTED_STRING, "Invalid escape sequence");
            return JSON_PARSER_STATE_ERROR;
        }

        start++;
    }

    // null terminate the string
    string[string_index] = '\0';
    element->type = JSON_STRING;
    element->value.string_val = string;
    return JSON_PARSER_STATE_OK;
}

// Consider that the prefix is already skipped
static JsonParserStateResult _parse_number_base(JsonParserState *state, double *result, const char *base)
{
    const char *pos = NULL;
    const size_t base_size = strlen(base);
    unsigned long value = 0;

    // check the first character then parse till it's valid
    if (strchr(base, tolower(_get_char_at_position(state))) == NULL) {
        _set_error_at_position(state, JSON_ERROR_INVALID_NUMBER, "Number has found an invalid digit as first character");
        return JSON_PARSER_STATE_ERROR;
    }

    while ((pos = strchr(base, tolower(_get_char_at_position(state)))) != NULL && _get_char_at_position(state) != '\0') {
        const size_t numerical_value = (size_t)pos - (size_t)base;
        value = value * base_size + numerical_value;
        state->position++;
    }

    *result = (double)value;
    return JSON_PARSER_STATE_OK;
}

static JsonParserStateResult _parse_number(JsonParserState *state, JsonElement *element)
{
    static const char *number_extensions[] = { "0x", "0b", "0o" };
    static const char *number_extensions_bases[] = { "0123456789abcdef", "01", "01234567" };

    size_t start = state->position;
    bool minus = false;
    const char *number_base = "0123456789";
    bool used_extension = false;

    if (_get_char_at_position(state) == '-' || _get_char_at_position(state) == '+') {
        minus = _get_char_at_position(state) == '-';
        state->position++;
    }

    // In case current value is not a digit
    if (isdigit(_get_char_at_position(state)) == 0) {
        if (start != state->position) {
            _set_error_at_position(state, JSON_ERROR_INVALID_NUMBER, "Found + or - but no numerical value afterwards");
            return JSON_PARSER_STATE_ERROR;
        }
        return JSON_PARSER_STATE_CONTINUE; // Simply not a number so continue
    }

    // In case extensions are turned on check for them before parsing a number
    if (state->options.allow_number_extensions) {
        for (size_t i = 0; i < sizeof(number_extensions) / sizeof(*number_extensions); ++i) {
            if (_is_token_at_position(state, number_extensions[i])) {
                state->position += strlen(number_extensions[i]);
                number_base = number_extensions_bases[i];
                used_extension = true;
                break;
            }
        }
    }

    double round = 0.0;
    double decimal = 0.0;
    double number;
    if (_parse_number_base(state, &round, number_base) == JSON_PARSER_STATE_ERROR) {
        return JSON_PARSER_STATE_ERROR;
    }

    // When an extension is used do not parse exponentials and/or floating points values
    if (!used_extension) {
        // calculate the floating value
        if (_get_char_at_position(state) == '.') {
            state->position++;
            if (_parse_number_base(state, &decimal, number_base) == JSON_PARSER_STATE_ERROR) {
                return JSON_PARSER_STATE_ERROR;
            }
            while (decimal >= 1.0) decimal /= 10.0;
        }

        number = round + decimal;

        // calculate the exponent
        if (_get_char_at_position(state) == 'e') {
            double _exp = 0.0;
            bool _exp_minus = false;
            state->position++;

            if (_get_char_at_position(state) == '-' || _get_char_at_position(state) == '+') {
                _exp_minus = _get_char_at_position(state) == '-';
                state->position++;
            }

            if (_parse_number_base(state, &_exp, number_base) == JSON_PARSER_STATE_ERROR) {
                return JSON_PARSER_STATE_ERROR;
            }
            double factor = _exp_minus ? 0.1 : 10.0;
            for (unsigned int i = 0; i < _exp; ++i) number *= factor;
        }
    } else {
        number = decimal;
    }

    // reverse the number if it should be negative and setup the element type
    element->value.number_val = minus ? -number : number;
    element->type = JSON_NUMBER;

    return JSON_PARSER_STATE_OK;
}

static JsonParserStateResult _parse_value(JsonParserState *, JsonElement *);

static JsonParserStateResult _parse_array(JsonParserState *state, JsonElement *element)
{
    if (!_is_char_at_position(state, '[')) {
        return JSON_PARSER_STATE_CONTINUE;
    }

    state->position++;
    Vector *array;
    JSON_CREATE_VECTOR_CHECKED(array, {}, JSON_PARSER_STATE_ERROR);

    _parse_comment(state);
    if (_is_char_at_position(state, ']')) {
        state->position++;
        element->type = JSON_ARRAY;
        element->value.array_val = array;
        return JSON_PARSER_STATE_OK;
    }

    while (state->position < state->input_len) {
        JsonElement *value;
        JSON_CREATE_CHECKED(value, JSON_NULL, { vector_deallocate(array); }, JSON_PARSER_STATE_ERROR);
        JSON_PARSE_VALUE_CHECKED(state, value, { vector_deallocate(array); json_deallocate(value); }, JSON_PARSER_STATE_OK, JSON_PARSER_STATE_ERROR);
        JSON_CREATE_VECTOR_PUSH_BACK_CHECKED(array, &value, { vector_deallocate(array); json_deallocate(value); }, JSON_PARSER_STATE_ERROR);

        _parse_comment(state);
        if (_is_char_at_position(state, ',')) {
            state->position++;
            _parse_comment(state);
        } else if (_is_char_at_position(state, ']')) {
            state->position++;
            element->type = JSON_ARRAY;
            element->value.array_val = array;
            return JSON_PARSER_STATE_OK;
        } else {
            _set_error_at_position(state, JSON_ERROR_UNFINISHED_ARRAY, "Unfinished array");
            vector_deallocate(array);
            return JSON_PARSER_STATE_ERROR;
        }
    }

    _set_error_at_position(state, JSON_ERROR_UNFINISHED_ARRAY, "Unfinished array");
    vector_deallocate(array);
    return JSON_PARSER_STATE_ERROR;
}



static JsonParserStateResult _parse_object(JsonParserState *state, JsonElement *element)
{
    if (!_is_char_at_position(state, '{')) {
        return JSON_PARSER_STATE_CONTINUE;
    }

    state->position++;

    Map *object;
    JSON_CREATE_MAP_CHECKED(object, {}, JSON_PARSER_STATE_ERROR);

    _parse_comment(state);
    if (_is_char_at_position(state, '}')) {
        state->position++;
        element->type = JSON_OBJECT;
        element->value.object_val = object;
        return JSON_PARSER_STATE_OK;
    }

    while (state->position < state->input_len) {
        char *key = NULL;

        JsonElement *jkey;
        JSON_CREATE_CHECKED(jkey, JSON_STRING, { map_deallocate(object); }, JSON_PARSER_STATE_ERROR)

        JsonParserStateResult result = _parse_string(state, jkey);
        switch (result) {
            case JSON_PARSER_STATE_ERROR: {
                json_deallocate(jkey);
                map_deallocate(object);
                return JSON_PARSER_STATE_ERROR;
            }
            case JSON_PARSER_STATE_OK: {
                JSON_CREATE_STRDUP_CHECKED(key, jkey->value.string_val, { json_deallocate(jkey); map_deallocate(object); }, JSON_PARSER_STATE_ERROR);
                json_deallocate(jkey);
                break;
            }
            case JSON_PARSER_STATE_CONTINUE: {
                if (state->options.allow_unquoted_keys) {
                    // parse the key until we find a colon
                    if (isalpha(_get_char_at_position(state)) == 0) {
                        _set_error_at_position(state, JSON_ERROR_UNFINISHED_OBJECT, "Unfinished object");
                        map_deallocate(object);
                        return JSON_PARSER_STATE_ERROR;
                    }

                    size_t start = state->position;
                    while (_get_char_at_position(state) != ':') {
                        state->position++;
                        if (state->position >= state->input_len || isalpha(_get_char_at_position(state)) == 0) {
                            _set_error_at_position(state, JSON_ERROR_UNFINISHED_OBJECT, "Invalid Non-quoted key");
                            free(key);
                            map_deallocate(object);
                            return JSON_PARSER_STATE_ERROR;
                        }
                    }
                    // copy the key without ':'
                    JSON_CREATE_STRNDUP_CHECKED(key, state->input + start, state->position - start,
                                                { map_deallocate(object); }, JSON_PARSER_STATE_ERROR);

                } else {
                    _set_error_at_position(state, JSON_ERROR_UNFINISHED_OBJECT, "Non-quoted key is not enabled");
                    map_deallocate(object);
                    return JSON_PARSER_STATE_ERROR;
                }
            }
        }

        _parse_comment(state);
        if (!_is_char_at_position(state, ':')) {
            _set_error_at_position(state, JSON_ERROR_UNFINISHED_OBJECT_KEY, "Unfinished key-value pair");
            free(key);
            map_deallocate(object);
            return JSON_PARSER_STATE_ERROR;
        }
        state->position++;

        JsonElement *value;
        JSON_CREATE_CHECKED(value, JSON_NULL,
            { free(key); json_deallocate(value); map_deallocate(object); },
            JSON_PARSER_STATE_ERROR);
        JSON_PARSE_VALUE_CHECKED(state, value,
            { free(key); json_deallocate(value); map_deallocate(object); },
            JSON_PARSER_STATE_OK, JSON_PARSER_STATE_ERROR);
        JSON_CREATE_MAP_INSERT_CHECKED(object, key, value,
            { free(key); json_deallocate(value); map_deallocate(object); },
            JSON_PARSER_STATE_ERROR);

        _parse_comment(state);

        if (_is_char_at_position(state, ',')) {
            state->position++;
            _parse_comment(state);
        } else if (_is_char_at_position(state, '}')) {
            state->position++;
            element->type = JSON_OBJECT;
            element->value.object_val = object;
            return JSON_PARSER_STATE_OK;
        } else {
            _set_error_at_position(state, JSON_ERROR_UNFINISHED_OBJECT, "Unfinished object");
            map_deallocate(object); // will deallocate the key and value because they are in the map
            return JSON_PARSER_STATE_ERROR;
        }
    }
}

static JsonParserStateResult _parse_comment_proxy(JsonParserState *state, JsonElement *element)
{
    (void)element;
    return _parse_comment(state);
}

static JsonParserStateResult _parse_value(JsonParserState *state, JsonElement *element)
{
    JsonParserStateResult (*parsers[])(JsonParserState *, JsonElement *) = {
        _parse_comment_proxy,
        _parse_null,
        _parse_boolean,
        _parse_string,
        _parse_number,
        _parse_array,
        _parse_object
    };

    for (size_t i = 0; i < sizeof(parsers) / sizeof(*parsers); i++) {
        JsonParserStateResult result = parsers[i](state, element);
        if (result != JSON_PARSER_STATE_CONTINUE) {
            return result;
        }
    }

    _set_error_at_position(state, JSON_ERROR_NO_PARSERS_FOUND, "Could not parse either null, boolean, string, number, array or object");
}

JsonElement* json_parse_with_options(const char *json_str, JsonParseOptions options)
{
    if (options.strict) {
        JsonParseOptions _strict = {0};
        _strict.strict = true;

        if (memcmp(&options, &_strict, sizeof(JsonParseOptions)) != 0) {
            fprintf(stderr, "Warning: strict is enabled but other options are set overriding them\n");
        }
        options = _strict;
    }

    if (options.allow_trailing_comma) {
        fprintf(stderr, "Warning: allow_trailing_comma is not implemented yet\n");
        return NULL;
    }

    if (options.allow_utf8) {
        fprintf(stderr, "Warning: allow_utf8 is not implemented yet\n");
        return NULL;
    }

    JsonParserState state = {
        .input = json_str,
        .input_len = strlen(json_str),
        .position = 0,
        .options = options
    };

    JsonElement *element;
    JSON_CREATE_CHECKED(element, JSON_NULL, {}, NULL);
    JSON_PARSE_VALUE_CHECKED(&state, element, { json_deallocate(element); }, JSON_PARSER_STATE_OK, NULL);

    _parse_comment(&state);
    if (!_is_position_at_end(&state)) {
        _set_error_at_position(&state, JSON_ERROR_FINISHED_EARLY, "Unexpected trailing characters");
        json_deallocate(element);
        return NULL;
    }

    return element;
}



JsonElement* json_read_from_file_with_options(const char *filename, JsonParseOptions options)
{
    FILE *file = fopen(filename, "r");
    if (file == NULL) {
        _set_last_error(JSON_ERROR_FILE_NOT_FOUND, "File not found");
        return NULL;
    }

    fseek(file, 0, SEEK_END);
    size_t file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    char *buffer;
    _JSON_CREATE_ALLOCATOR_CHECKED(buffer, malloc(file_size + 1), { fclose(file); }, NULL);

    size_t read = fread(buffer, 1, file_size, file);
    if (read != file_size) {
        _set_last_error(JSON_ERROR_IO_ERROR, "IO error");
        fclose(file);
        free(buffer);
        return NULL;
    }

    buffer[file_size] = '\0';
    fclose(file);

    JsonElement *element = json_parse_with_options(buffer, options);
    free(buffer);
    return element;
}

JsonElement* json_parse(const char *json_str)
{
    return json_parse_with_options(json_str, json_default_parse_options);
}

JsonElement *json_read_from_file(const char *filename)
{
    return json_read_from_file_with_options(filename, json_default_parse_options);
}

// Getters
JsonElement *json_get_array_element(const JsonElement *array, size_t index)
{
    JSON_CREATE_TYPE_CHECK(array, JSON_ARRAY, {}, NULL);

    if (index >= vector_size(array->value.array_val)) {
        _set_last_error(JSON_ERROR_OUT_OF_RANGE, "Out of range");
        return NULL;
    }

    return *(JsonElement **)vector_at(array->value.array_val, index);
}

JsonElement *json_get_object_element(const JsonElement *object, const char *key)
{
    JSON_CREATE_TYPE_CHECK(object, JSON_OBJECT, {}, NULL);
    return (JsonElement *)map_at(object->value.object_val, (void*)(key));
}

JsonElement *json_get_element(const JsonElement *element, const char *key_or_index)
{
    if (element->type == JSON_ARRAY) {
        // check all the characters are digits
        size_t key_or_index_len = strlen(key_or_index);

        if (_all_is_str(key_or_index, isdigit) == false) {
            _set_last_error(JSON_ERROR_INVALID_QUERY, "Invalid query expecting a number");
            return NULL;
        }

        size_t index = strtoul(key_or_index, NULL, 10);
        return json_get_array_element(element, index);
    } else if (element->type == JSON_OBJECT) {
        return json_get_object_element(element, key_or_index);
    }
    _set_last_error(JSON_ERROR_INVALID_TYPE, "Invalid type expecting an array or an object");
    return NULL;
}

JsonElement *json_query(const JsonElement *element, const char *query)
{
    if (element == NULL) {
        return NULL;
    }

    switch (query[0]) {
        case '\0': {
            return (JsonElement *)element;
        }
        case '.': {
            if (query[1] == '\0') {
                return (JsonElement *)element;
            }

            char *ptr = strchr(query, '.');

            if (ptr == NULL) {
                ptr = strchr(query, '[');
            }

            if (ptr == NULL) {
                ptr = strchr(query, '\0');
            }

            char *key;
            JSON_CREATE_STRNDUP_CHECKED(key, query, ptr - query, {}, NULL);
            JsonElement *sub_element = json_get_element(element, key);
            free(key);
            return json_query(sub_element, ptr);
        }

        case '[': {
            if (query[1] == '\0') {
                return (JsonElement *)element;
            }

            char *ptr = strchr(query, ']');
            if (ptr == NULL) {
                // non terminated query
                _set_last_error(JSON_ERROR_INVALID_QUERY, "Invalid query using '[' without terminating ']'");
                return NULL;
            }

            char *key;
            JSON_CREATE_STRNDUP_CHECKED(key, query + 1, ptr - query - 1, {}, NULL);

            JsonElement *sub_element = json_get_element(element, key);
            free(key);
            return json_query(sub_element, ptr + 1);
        }
    
        default: {
            _set_last_error(JSON_ERROR_INVALID_QUERY, "Invalid query expecting '.' or '['");
            return NULL;
        }
    }
}

JsonElement *json_find(const JsonElement *element, JsonPredicate predicate, void *user_data);

static bool json_find_in_object(const JsonElement *element, JsonPredicate predicate, void *user_data, JsonElement **result)
{
    JSON_CREATE_TYPE_CHECK(*result, JSON_OBJECT, {}, false);

    MAP_FOREACH(void *, _, JsonElement *, value, element->value.object_val, {
        if (predicate(value, user_data)) {
            *result = value;
            return true;
        }
    });

    return false;
}

static bool json_find_in_array(const JsonElement *element, JsonPredicate predicate, void *user_data, JsonElement **result)
{
    JSON_CREATE_TYPE_CHECK(*result, JSON_ARRAY, {}, false);

    VECTOR_FOREACH(JsonElement *, element->value.array_val, it, {
        if (predicate(*it, user_data)) {
            *result = *it;
            return true;
        }
    });

    return false;
}

JsonElement *json_find(const JsonElement *element, JsonPredicate predicate, void *user_data)
{
    if (!element || !predicate) {
        _set_last_error(JSON_ERROR_INVALID_ARGUMENT, "Invalid argument expecting a non null element and predicate");
        return NULL;
    }

    JsonElement *result = NULL;
    switch (element->type) {
        case JSON_OBJECT:
            if (json_find_in_object(element, predicate, user_data, &result)) {
                return result;
            }
            break;
        case JSON_ARRAY:
            if (json_find_in_array(element, predicate, user_data, &result)) {
                return result;
            }
            break;
        default:
            _set_last_error(JSON_ERROR_INVALID_TYPE, "Invalid type expecting an array or an object for json_find");
            return NULL;
    }
    return NULL;
}

JsonElement* json_merge(const JsonElement *element1, const JsonElement *element2)
{
    if (!element1 || !element2 || element1->type != JSON_OBJECT || element2->type != JSON_OBJECT) {
        _set_last_error(JSON_ERROR_INVALID_TYPE, "Invalid type expecting two objects");
        return NULL;
    }

    JsonElement* merged;
    JSON_CREATE_CHECKED(merged, JSON_OBJECT, {}, NULL);

    // Insert all elements from the first object into the merged object
    MAP_FOREACH(char *, pkey, JsonElement *, value, element1->value.object_val, {
        char* key;
        JsonElement* value_copy;
        JSON_CREATE_STRDUP_CHECKED(key, pkey, { json_deallocate(merged); }, NULL);
        JSON_CREATE_DEEP_COPY_CHECKED(value_copy, value, { free(key); json_deallocate(merged); }, NULL);
        JSON_CREATE_MAP_INSERT_CHECKED(merged->value.object_val, key, value_copy,
                                    { free(key); json_deallocate(merged); }, NULL);
    });

    // Merge elements from the second object
    MAP_FOREACH(char *, pkey, JsonElement *, value, element2->value.object_val, {
        char* key;
        JsonElement* value_copy;
        JSON_CREATE_STRDUP_CHECKED(key, pkey, { json_deallocate(merged); }, NULL);
        JSON_CREATE_DEEP_COPY_CHECKED(value_copy, value, { free(key); json_deallocate(merged); }, NULL);
        JSON_CREATE_MAP_INSERT_CHECKED(merged->value.object_val, key, value_copy,
                                    { free(key); json_deallocate(merged); }, NULL);
    });

    return merged;
}

JsonElement *json_filter(const JsonElement *array, JsonPredicate predicate, void *user_data)
{
    if (!array || !predicate) {
        _set_last_error(JSON_ERROR_INVALID_ARGUMENT, "Invalid argument expecting a non null array and predicate");
        return NULL;
    }
    JSON_CREATE_TYPE_CHECK(array, JSON_ARRAY, {}, NULL);

    JsonElement *filtered;
    JSON_CREATE_CHECKED(filtered, JSON_ARRAY, {}, NULL);
    VECTOR_FOREACH(JsonElement *, array->value.array_val, it, {
        if (predicate(*it, user_data)) {
            JsonElement *item_copy;
            JSON_CREATE_DEEP_COPY_CHECKED(item_copy, *it,
                                        { json_deallocate(filtered); }, NULL);
            JSON_CREATE_VECTOR_PUSH_BACK_CHECKED(filtered->value.array_val, &item_copy,
                                        { json_deallocate(filtered); }, NULL);
        }
    });

    return filtered;
}

// Bad implementation should instead be printed
// using a buffer like FILE* that flushes so it avoids too large strings
void json_print(JsonElement *element, int ident)
{
    char *str = json_serialize(element, ident);
    if (str == NULL) {
        fmt_fprintf(stderr, "Error: %s\n", json_last_error());
        return;
    }
    fmt_printf("%s", str);
    free(str);
}

bool json_compare(const JsonElement *element1, const JsonElement *element2)
{
    if (element1 == element2) {
        return true;
    }

    if (element1 == NULL || element2 == NULL || (element1->type != element2->type)) {
        return false;
    }

    switch (element1->type) {
    case JSON_NULL:
        return true;
    case JSON_BOOL:
        return element1->value.bool_val == element2->value.bool_val;
    case JSON_STRING:
        return strcmp(element1->value.string_val, element2->value.string_val) == 0;
    case JSON_NUMBER:
        return element1->value.number_val == element2->value.number_val;
    case JSON_ARRAY:
        if (vector_size(element1->value.array_val) != vector_size(element2->value.array_val)) {
            return false;
        }
        VECTOR_FOREACH(JsonElement *, element1->value.array_val, it, {
            const size_t index = (size_t)it - (size_t)vector_begin(element1->value.array_val);
            JsonElement **value2 = (JsonElement **)vector_at(element2->value.array_val, index);
            if (value2 == NULL || !json_compare(*it, *value2)) {
                return false;
            }
        });
        return true;
    case JSON_OBJECT:
        if (map_size(element1->value.object_val) != map_size(element2->value.object_val)) {
            return false;
        }
        MAP_FOREACH(char *, key, JsonElement *, value, element1->value.object_val, {
            JsonElement *value2 = (JsonElement *)map_at(element2->value.object_val, key);
            if (value2 == NULL || !json_compare(value, value2)) {
                return false;
            }
        });
        return true;
    }
    return false;
}

JsonElement *json_map(const JsonElement *array, JsonMapFunction function, void *user_data)
{
    if (!array || !function) {
        _set_last_error(JSON_ERROR_INVALID_ARGUMENT, "Invalid argument expecting a non null array and function");
        return NULL;
    }
    JSON_CREATE_TYPE_CHECK(array, JSON_ARRAY, {}, NULL);

    JsonElement *mapped;
    JSON_CREATE_CHECKED(mapped, JSON_ARRAY, {}, NULL);
    VECTOR_FOREACH(JsonElement *, array->value.array_val, it, {
        JsonElement *item = function(*it, user_data);
        if (item == NULL) {
            json_deallocate(mapped);
            return NULL;
        }
        JSON_CREATE_VECTOR_PUSH_BACK_CHECKED(mapped->value.array_val, &item, { json_deallocate(mapped); }, NULL);
    });

    return mapped;
}

static char *_append_str(char *str, const char *append, size_t *len, size_t *size)
{
    size_t append_len = strlen(append);
    if (*len + append_len + 1 > *size) {
        size_t tmp = 1;
        for (; tmp <= *len + append_len + 1; tmp <<= 1);
        *size = tmp;
        _JSON_CREATE_ALLOCATOR_CHECKED(str, realloc(str, *size), {}, NULL);
    }
    memcpy(str + *len, append, append_len);
    *len += append_len;
    str[*len] = '\0';
    return str;
}

inline static char *_ident_append(char *str, int ident, int depth, size_t *len, size_t *size)
{
    if (ident == 0) {
        return str;
    }

    size_t ident_len = ident * depth;

    if (*len + ident_len + 2 > *size) {
        size_t tmp = 1;
        for (; tmp <= *len + ident_len + 2; tmp <<= 1);
        *size = tmp;
        _JSON_CREATE_ALLOCATOR_CHECKED(str, realloc(str, *size), {}, NULL);
    }

    str[*len] = '\n';
    str[*len + ident_len + 1] = '\0';
    memset(str + 1 + *len, ' ', ident_len);
    *len += ident_len + 1;
    return str;
}

static char *json_serialize_r(const JsonElement *element, int ident, int depth, char *str, size_t *len, size_t *size)
{
    switch (element->type) {
        case JSON_NULL: {
            _JSON_SERIALIZE_APPEND_STR(str, "null", len, size);
            return str;
        }
        case JSON_BOOL: {
            _JSON_SERIALIZE_APPEND_STR(str, element->value.bool_val ? "true" : "false", len, size);
            return str;
        }
        case JSON_STRING: {
            _JSON_SERIALIZE_APPEND_STR(str, "\"", len, size);
            _JSON_SERIALIZE_APPEND_STR(str, element->value.string_val, len, size);
            _JSON_SERIALIZE_APPEND_STR(str, "\"", len, size);
            return str;
        }
        case JSON_NUMBER: {
            char number[64];
            snprintf(number, sizeof(number), "%g", element->value.number_val);
            _JSON_SERIALIZE_APPEND_STR(str, number, len, size);
            return str;
        }
        case JSON_ARRAY: {
            _JSON_SERIALIZE_APPEND_STR(str, "[", len, size);
            VECTOR_FOREACH(JsonElement *, element->value.array_val, it, {
                if (it != vector_begin(element->value.array_val)) {
                    _JSON_SERIALIZE_APPEND_STR(str, ",", len, size);
                }
                _JSON_SERIALIZE_APPEND_IDENT(str, ident, depth, len, size);
                _JSON_SERIALIZE_R_CHECKED(*it, ident, depth + 1, str, len, size);
            });
            _JSON_SERIALIZE_APPEND_IDENT(str, ident, depth - 1, len, size);
            _JSON_SERIALIZE_APPEND_STR(str, "]", len, size);
            return str;
        }
        case JSON_OBJECT: {
            _JSON_SERIALIZE_APPEND_STR(str, "{", len, size);
            bool second = false;
            MAP_FOREACH(char *, key, JsonElement *, value, element->value.object_val, {
                if (second) {
                    _JSON_SERIALIZE_APPEND_STR(str, ",", len, size);
                } else {
                    second = true;
                }
                _JSON_SERIALIZE_APPEND_IDENT(str, ident, depth, len, size);
                _JSON_SERIALIZE_APPEND_STR(str, "\"", len, size);
                _JSON_SERIALIZE_APPEND_STR(str, key, len, size);
                _JSON_SERIALIZE_APPEND_STR(str, "\":", len, size);
                _JSON_SERIALIZE_R_CHECKED(value, ident, depth + 1, str, len, size);
            });
            _JSON_SERIALIZE_APPEND_IDENT(str, ident, depth - 1, len, size);
            _JSON_SERIALIZE_APPEND_STR(str, "}", len, size);
            return str;
        }
    }
    return str;
}

char *json_serialize(const JsonElement *element, int ident)
{
    size_t len = 0;
    size_t size = 64;
    char *str;

    if (element == NULL) {
        _set_last_error(JSON_ERROR_INVALID_ARGUMENT, "Invalid argument expecting a non null element");
        return NULL;
    }

    // allocate a string of a reasonable size to start with
    if (element->type == JSON_OBJECT || element->type == JSON_ARRAY) {
        size_t current_size = element->type == JSON_OBJECT ? map_size(element->value.object_val) : vector_size(element->value.array_val);
        size = 64 * current_size + ident * current_size + 1;
        size_t v = 1;
        // find the next power of 2
        for (; v <= size; v <<= 1);
        size = v;
    }

    _JSON_CREATE_ALLOCATOR_CHECKED(str, malloc(size), {}, NULL);
    _JSON_SERIALIZE_R_CHECKED(element, ident, 1, str, &len, &size);
    return str;
}

bool json_write_to_file(const JsonElement *element, const char *filename)
{
    char *str = json_serialize(element, 0);
    if (str == NULL) {
        return false;
    }

    FILE *file = fopen(filename, "w+");
    if (file == NULL) {
        free(str);
        _set_last_error(JSON_ERROR_FILE_NOT_FOUND, "File not found");
        return false;
    }

    size_t written = fwrite(str, 1, strlen(str), file);
    fclose(file);
    free(str);
    if (written != strlen(str)) {
        _set_last_error(JSON_ERROR_IO_ERROR, "IO error");
        return false;
    }
    return true;
}

bool json_set_element_array(JsonElement *array, size_t index, JsonElement *element)
{
    JSON_CREATE_TYPE_CHECK(array, JSON_ARRAY, {}, false);

    if (index >= vector_size(array->value.array_val)) {
        _set_last_error(JSON_ERROR_OUT_OF_RANGE, "Out of range");
        return false;
    }

    JsonElement *old = *(JsonElement **)vector_at(array->value.array_val, index);
    json_deallocate(old);
    *(JsonElement **)vector_at(array->value.array_val, index) = element;
    return true;
}

bool json_set_element_object(JsonElement *object, const char *key, JsonElement *element)
{
    JSON_CREATE_TYPE_CHECK(object, JSON_OBJECT, {}, false);

    JsonElement *old = (JsonElement *)map_at(object->value.object_val, (void*)(key));
    if (old != NULL) {
        json_deallocate(old);
    }
    JSON_CREATE_MAP_INSERT_CHECKED(object->value.object_val, (void*)(key), element, {}, false);
    return true;
}

bool json_set_element(JsonElement *element, const char *key_or_index, JsonElement *value)
{
    if (element->type == JSON_ARRAY) {
        // check all the characters are digits
        if (_all_is_str(key_or_index, isdigit) == false) {
            _set_last_error(JSON_ERROR_INVALID_QUERY, "Invalid query expecting a number");
            return false;
        }
        size_t index = strtoul(key_or_index, NULL, 10);
        return json_set_element_array(element, index, value);
    } else if (element->type == JSON_OBJECT) {
        return json_set_element_object(element, key_or_index, value);
    }
    _set_last_error(JSON_ERROR_INVALID_TYPE, "Invalid type expecting an array or an object");
    return false;
}

void *json_reduce(const JsonElement *array, JsonReduceFunction function, void *initial, void *user_data)
{
    if (!array || !function) {
        _set_last_error(JSON_ERROR_INVALID_ARGUMENT, "Invalid argument expecting a non null array and function");
        return NULL;
    }
    JSON_CREATE_TYPE_CHECK(array, JSON_ARRAY, {}, NULL);

    void *accumulator = initial;
    VECTOR_FOREACH(JsonElement *, array->value.array_val, it, {
        accumulator = function(accumulator, *it, user_data);
    });

    return accumulator;
}

bool json_remove_element_array(JsonElement *array, size_t index)
{
    JSON_CREATE_TYPE_CHECK(array, JSON_ARRAY, {}, false);

    if (index >= vector_size(array->value.array_val)) {
        _set_last_error(JSON_ERROR_OUT_OF_RANGE, "Out of range");
        return false;
    }

    JsonElement *element = *(JsonElement **)vector_at(array->value.array_val, index);
    json_deallocate(element);
    vector_erase(array->value.array_val, index, sizeof(JsonElement *));
    return true;
}

bool json_remove_element_object(JsonElement *object, const char *key)
{
    JSON_CREATE_TYPE_CHECK(object, JSON_OBJECT, {}, false);

    JsonElement *element = (JsonElement *)map_at(object->value.object_val, (void*)(key));
    if (element == NULL) {
        _set_last_error(JSON_ERROR_OUT_OF_RANGE, "Out of range");
        return false;
    }
    json_deallocate(element);
    map_erase(object->value.object_val, (void*)(key));
    return true;
}

bool json_remove_element(JsonElement *element, const char *key_or_index)
{
    if (element->type == JSON_ARRAY) {
        // check all the characters are digits
        if (_all_is_str(key_or_index, isdigit) == false) {
            _set_last_error(JSON_ERROR_INVALID_QUERY, "Invalid query expecting a number");
            return false;
        }
        size_t index = strtoul(key_or_index, NULL, 10);
        return json_remove_element_array(element, index);
    } else if (element->type == JSON_OBJECT) {
        return json_remove_element_object(element, key_or_index);
    }
    _set_last_error(JSON_ERROR_INVALID_TYPE, "Invalid type expecting an array or an object");
    return false;
}

static JsonElement *json_convert_null_to(const JsonElement *element, JsonType type)
{
    JsonElement *converted;

    JSON_CREATE_CHECKED(converted, type, {}, NULL);

    switch (type) {
        case JSON_BOOL: {
            converted->value.bool_val = false;
            break;
        }
        case JSON_STRING: {
            JSON_CREATE_STRDUP_CHECKED(converted->value.string_val, "null", { json_deallocate(converted); }, NULL);
            break;
        }
        case JSON_NUMBER: {
            converted->value.number_val = 0;
            break;
        }

        case JSON_NULL:
        case JSON_ARRAY:
        case JSON_OBJECT:
            break;
    }

    return converted;
}

static JsonElement *json_convert_boolean_to(const JsonElement *element, JsonType type)
{
    JsonElement *converted;

    switch (type) {
        case JSON_NULL: {
            if (element->value.bool_val) {
                _set_last_error(JSON_ERROR_INVALID_TYPE, "Invalid type cannot transform true to null");
                return NULL;
            }
            JSON_CREATE_CHECKED(converted, JSON_NULL, {}, NULL);
            return converted;
        }
        case JSON_BOOL: {
            return json_deep_copy(element);
        }
        case JSON_STRING: {
            JSON_CREATE_CHECKED(converted, JSON_STRING, {}, NULL);
            JSON_CREATE_STRDUP_CHECKED(converted->value.string_val, element->value.bool_val ? "true" : "false", { json_deallocate(converted); }, NULL);
            return converted;
        }
        case JSON_NUMBER: {
            JSON_CREATE_CHECKED(converted, JSON_NUMBER, {}, NULL);
            converted->value.number_val = element->value.bool_val ? 1 : 0;
            return converted;
        }
        case JSON_ARRAY:
        case JSON_OBJECT: {
            // To number to array
            JsonElement *number = json_convert(element, JSON_NUMBER);
            if (number == NULL) {
                return NULL;
            }
            JsonElement *array = json_convert(number, type);
            json_deallocate(number);
            return array;
        }
    }
}

static JsonElement *json_convert_string_to(const JsonElement *element, JsonType type)
{
    JsonElement *converted;

    if (type == JSON_STRING) {
        return json_deep_copy(element);
    }
    
    converted = json_parse(element->value.string_val);
    if (converted == NULL) {
        return NULL;
    }

    void *result = json_convert(converted, type); // Magic happens here (not really but it's a good comment)
    json_deallocate(converted);
    return result;
}

static JsonElement *json_convert_number_to(const JsonElement *element, JsonType type)
{
    JsonElement *converted;

    switch (type) {
        case JSON_NULL: {
            if (element->value.number_val == 0) {
                JSON_CREATE_CHECKED(converted, JSON_NULL, {}, NULL);
                return converted;
            }
            _set_last_error(JSON_ERROR_INVALID_TYPE, "Invalid type cannot transform non-zero number to null");
            return NULL;
        }
        case JSON_BOOL: {
            JSON_CREATE_CHECKED(converted, JSON_BOOL, {}, NULL);
            converted->value.bool_val = element->value.number_val != 0;
            return converted;
        }
        case JSON_STRING: {
            char s_n[64];
            snprintf(s_n, sizeof(s_n), "%g", element->value.number_val);
            JSON_CREATE_CHECKED(converted, JSON_STRING, {}, NULL);
            JSON_CREATE_STRDUP_CHECKED(converted->value.string_val, s_n, { json_deallocate(converted); }, NULL);
            return converted;
        }
        case JSON_NUMBER: {
            return json_deep_copy(element);
        }
        case JSON_ARRAY: {
            JSON_CREATE_CHECKED(converted, JSON_ARRAY, {}, NULL);
            JsonElement *element_copy;
            _JSON_CREATE_ALLOCATOR_CHECKED(element_copy, json_deep_copy(element), { json_deallocate(converted); }, NULL);
            JSON_CREATE_VECTOR_PUSH_BACK_CHECKED(converted->value.array_val, &element_copy, {
                json_deallocate(converted);
                json_deallocate(element_copy);
            }, NULL);
            return converted;
        }
        case JSON_OBJECT: {
            JSON_CREATE_CHECKED(converted, JSON_OBJECT, {}, NULL);
            JsonElement *element_copy;
            _JSON_CREATE_ALLOCATOR_CHECKED(element_copy, json_deep_copy(element), { json_deallocate(converted); }, NULL);
            JSON_CREATE_MAP_INSERT_CHECKED(converted->value.object_val, "0", element_copy, {
                json_deallocate(converted);
                json_deallocate(element_copy);
            }, NULL);
            return converted;
        }
    }

    _set_last_error(JSON_ERROR_INVALID_TYPE, "Invalid type could not convert number to the requested type");
    return NULL;
}

static JsonElement *json_convert_object_to(const JsonElement *element, JsonType type)
{
    switch (type) {
        case JSON_NULL: {
            _set_last_error(JSON_ERROR_INVALID_TYPE, "Invalid type object cannot be transformed to a null");
            return NULL;
        }
        case JSON_BOOL: {
            JsonElement *boolean;
            JSON_CREATE_CHECKED(boolean, JSON_BOOL, {}, NULL);
            boolean->value.bool_val = true;
            return boolean;
        }
        case JSON_STRING: {
            char *str = json_serialize(element, 0);
            if (str == NULL) {
                return NULL;
            }
            JsonElement *string;
            JSON_CREATE_CHECKED(string, JSON_STRING, { free(str); }, NULL);
            string->value.string_val = str;
            return string;
        }
        case JSON_NUMBER: {
            _set_last_error(JSON_ERROR_INVALID_TYPE, "Invalid type object cannot be transformed to a number");
            return NULL;
        }
        case JSON_ARRAY: {
            JsonElement *array;
            JSON_CREATE_CHECKED(array, JSON_ARRAY, {}, NULL);
            MAP_FOREACH(char *, key, JsonElement *, value, element->value.object_val, {
                JSON_CREATE_VECTOR_PUSH_BACK_CHECKED(array->value.array_val, &value, { json_deallocate(array); }, NULL);
            });
            return array;
        }
        case JSON_OBJECT: {
            return json_deep_copy(element);
        }
    }
}

static JsonElement *json_convert_array_to(const JsonElement *element, JsonType type)
{
    switch (type) {
        case JSON_BOOL: {
            JsonElement *boolean;
            JSON_CREATE_CHECKED(boolean, JSON_BOOL, {}, NULL);
            boolean->value.bool_val = true;
            return boolean;
        }
        case JSON_STRING: {
            char *str = json_serialize(element, 0);
            if (str == NULL) {
                return NULL;
            }
            JsonElement *string;
            JSON_CREATE_CHECKED(string, JSON_STRING, { free(str); }, NULL);
            string->value.string_val = str;
            return string;
        }
        case JSON_ARRAY: {
            return json_deep_copy(element);
        }
        case JSON_OBJECT: {
            JsonElement *object;
            JSON_CREATE_CHECKED(object, JSON_OBJECT, {}, NULL);
            VECTOR_FOREACH(JsonElement *, element->value.array_val, it, {
                char skey[64];
                char *akey;
                size_t index = (size_t)it - (size_t)vector_begin(element->value.array_val);
                snprintf(skey, sizeof(skey), "%zu", index);
                JSON_CREATE_STRDUP_CHECKED(akey, skey, { json_deallocate(object); }, NULL);
                JSON_CREATE_MAP_INSERT_CHECKED(object->value.object_val, akey, *it, { free(akey); json_deallocate(object); }, NULL);
            });
            return object;
        }
        case JSON_NULL:
        case JSON_NUMBER:
            _set_last_error(JSON_ERROR_INVALID_TYPE, "Invalid type expecting to transform array to (string, array, object, boolean)");
            return NULL;
    }
}

JsonElement *json_convert(const JsonElement *element, JsonType type)
{
    if (element == NULL) {
        _set_last_error(JSON_ERROR_INVALID_ARGUMENT, "Invalid argument expecting a non null element");
        return NULL;
    }

    if (element->type == type) {
        return json_deep_copy(element);
    }

    switch (element->type) {
        case JSON_NULL: {
            return json_convert_null_to(element, type);
        }
        case JSON_BOOL: {
            return json_convert_boolean_to(element, type);
        }
        case JSON_STRING: {
            return json_convert_string_to(element, type);
        }
        case JSON_NUMBER: {
            return json_convert_number_to(element, type);
        }
        case JSON_ARRAY: {
            return json_convert_array_to(element, type);
        }
        case JSON_OBJECT: {
            return json_convert_object_to(element, type);
        }
    }
}

#undef _JSON_CREATE_ALLOCATOR_CHECKED
#undef JSON_CREATE_CHECKED
#undef JSON_CREATE_VECTOR_CHECKED
#undef JSON_CREATE_MAP_CHECKED
#undef JSON_CREATE_STRDUP_CHECKED
#undef JSON_CREATE_STRNDUP_CHECKED
#undef JSON_CREATE_DEEP_COPY_CHECKED
#undef JSON_CREATE_VECTOR_PUSH_BACK_CHECKED
#undef JSON_CREATE_MAP_INSERT_CHECKED
#undef JSON_PARSE_VALUE_CHECKED
#undef JSON_CREATE_TYPE_CHECK
#undef _JSON_SERIALIZE_APPEND_STR
#undef _JSON_SERIALIZE_APPEND_IDENT
#undef _JSON_SERIALIZE_R_CHECKED
#undef _JSON_SERIALIZE_DEF