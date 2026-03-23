#define LUA_LIB

#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <lauxlib.h>
#include <lobject.h>
#include <ltable.h>
#include "qdef.h"
#include "qbuf.h"
#include "qbarray.h"

//type 2bit 0-3, value 6bit 0-63

//type 0, fixed length string 0-55
#define QSERI_TMAGIC 0
#define QSERI_TLARGESTRING 56
#define QSERI_TNEGINT 57
#define QSERI_TNIL 58
#define QSERI_TNUMBER 59
#define QSERI_TBOOLEANTRUE 60
#define QSERI_TBOOLEANFALSE 61
#define QSERI_TPOINTER 62
#define QSERI_TBUFFER 63

//type 1-3
#define QSERI_TUINT 1
#define QSERI_TARRAY 2
#define QSERI_TMAP 3

#define MAX_PACKAGE_SIZE 8388608 // about 8M
#define MAX_DEPTH 24
#define LARGE_TAG 63
#define BUFFER_INIT_SIZE 64

//0-62 small value, 63 large value
#define is_small_value(value) ((value)>=0 && (value)<=(LARGE_TAG-1))
#define combine_small_value(type, value) (uint8_t)(((type)<<6)|(value))

typedef struct qseri_string_key {
	const char* string;
	int32_t index;
} qseri_string_key;

typedef struct qseri_encoder {
	//encode buffer
	char* buffer;
	int32_t cap;
	int32_t write_ptr;
	//string key buffer
	char* string_key_buffer;
	int32_t string_key_cap;
	int32_t string_key_write_ptr;
	qbarray string_keys;
	int32_t* string_offsets;
	int32_t string_offset_cap;
	int32_t string_offset_write_ptr;
	bool compressible;
	char err_msg[128];
} qseri_encoder;

typedef struct qseri_qdecoder {
	//decode buffer
	char* buffer;
	int32_t size;
	int32_t read_ptr;
	int32_t* string_offsets;
	int32_t string_offset_count;
	bool compressible;
	char err_msg[128];
} qseri_qdecoder;

static bool table_info(lua_State* L, int32_t stack, bool* is_array, int32_t* size, qseri_encoder* encoder) {
	*is_array = true;
	int64_t max_key = -1;
	stack = lua_absindex(L, stack);
	if (luaL_getmetafield(L, stack, "__pairs") != LUA_TNIL) {
		lua_pushvalue(L, -2);
		if (lua_pcall(L, 1, 3, 0) == LUA_OK) {
			for(;;) {
				lua_pushvalue(L, -2);
				lua_pushvalue(L, -2);
				lua_copy(L, -5, -3);
				if (lua_pcall(L, 2, 2, 0) == LUA_OK) {
					int keyt = lua_type(L, -2);
					if (keyt == LUA_TNIL) {
						lua_pop(L, 4);
						break;
					}
					*size += 1;
					if (*is_array) {
						if (lua_type(L, -2) == LUA_TNUMBER && lua_isinteger(L, -2)) {
							int64_t key = lua_tointeger(L, -2);
							if (key < 1) {
								*is_array = false;
							}
							else {
								if (key > max_key) {
									max_key = key;
								}
							}
						}
						else {
							*is_array = false;
						}
					}
					lua_pop(L, 1);
				}
				else {
					snprintf(encoder->err_msg, sizeof(encoder->err_msg), lua_tostring(L, -1));
					lua_pop(L, 1);
					return false;
				}
			}
			if (*is_array) {
				*is_array = max_key == *size;
			}
		}
		else {
			snprintf(encoder->err_msg, sizeof(encoder->err_msg), lua_tostring(L, -1));
			lua_pop(L, 1);
			return false;
		}
	}
	else {
	  Table *t = (Table*)lua_topointer(L, stack);
	  int32_t arr_size = lua_rawlen(L, stack);
	  int32_t hsize = sizenode(t);
	  int32_t hused = 0;
	  int32_t i;
	  for (i = 0; i < hsize; i++) {
	      if (!ttisnil(gval(gnode(t, i)))) {
	          ++hused;
	      }
	  }
	  *size += arr_size + hused;
	  max_key = arr_size;
	  *is_array = (arr_size > 0) && (hused == 0);
	}
	return true;
}

static int32_t string_offset(qseri_encoder* encoder, const char* str) {
	qseri_string_key tmp;
	tmp.string = str; //lua string reference, it's safe during encoding
	tmp.index = -1;
	qseri_string_key* skey = (qseri_string_key*)qbarray_find(&encoder->string_keys, &tmp);
	if (skey) {
		return skey->index;
	}
	else {
		int32_t size = strlen(str) + 1;
		if ((encoder->string_key_write_ptr + size > encoder->string_key_cap)) {
			while (encoder->string_key_write_ptr + size > encoder->string_key_cap) {
				if (encoder->string_key_cap > 0) {
					encoder->string_key_cap <<= 1;
				}
				else {
					encoder->string_key_cap = size;
				}
			}
			encoder->string_key_buffer = realloc(encoder->string_key_buffer, encoder->string_key_cap);
		}
		int32_t offset = encoder->string_key_write_ptr;
		memcpy(encoder->string_key_buffer + offset, str, size);
		encoder->string_key_write_ptr += size;
		int32_t index = encoder->string_offset_write_ptr;
		tmp.index = index;
		if (encoder->string_offset_write_ptr >= encoder->string_offset_cap) {
			if (encoder->string_offset_cap > 0) {
				encoder->string_offset_cap <<= 1;
			}
			else {
				encoder->string_offset_cap = 4;
			}
			encoder->string_offsets = realloc(encoder->string_offsets, 
				encoder->string_offset_cap*sizeof(*encoder->string_offsets));
		}
		encoder->string_offsets[index] = offset;
		++encoder->string_offset_write_ptr;
		bool success = qbarray_insert(&encoder->string_keys, &tmp);
		assert(success);
		return index;
	}
}

static inline void write_type_value(qseri_encoder* encoder, uint8_t type, int64_t value) {
	if (is_small_value(value)) {
		write_byte(encoder, combine_small_value(type, value));
	}
	else {
		write_byte(encoder, combine_small_value(type, LARGE_TAG));
		write_uinteger(encoder, value);
	}
}

static bool encode_value(lua_State* L, int32_t stack, bool is_key, qseri_encoder* encoder, int32_t depth);

static bool encode_pair(lua_State* L, qseri_encoder* encoder, bool is_array, int32_t depth) {
	if (!is_array) {
		if (!encode_value(L, -2, true, encoder, depth)) {
			return false;
		}
	}
	if (!encode_value(L, -1, false, encoder, depth)) {
		return false;
	}
	return true;
}

static bool encode_table(lua_State* L, int32_t stack, qseri_encoder* encoder, int32_t size, bool is_array, int32_t depth) {
	if (depth > MAX_DEPTH) {
		snprintf(encoder->err_msg, sizeof(encoder->err_msg), "Attempt to pack an too depth table");
		return false;
	}
	write_type_value(encoder, is_array?QSERI_TARRAY:QSERI_TMAP, size);
	stack = lua_absindex(L, stack);
	if (luaL_getmetafield(L, stack, "__pairs") != LUA_TNIL) {
		lua_pushvalue(L, -2);
		if (lua_pcall(L, 1, 3, 0) == LUA_OK) {
			for(;;) {
				lua_pushvalue(L, -2);
				lua_pushvalue(L, -2);
				lua_copy(L, -5, -3);
				if (lua_pcall(L, 2, 2, 0) == LUA_OK) {
					int keyt = lua_type(L, -2);
					if (keyt == LUA_TNIL) {
						lua_pop(L, 4);
						break;
					}
					if (!encode_pair(L, encoder, is_array, depth+1)) {
						lua_pop(L, 4);
						return false;
					}
					lua_pop(L, 1);
				}
				else {
					snprintf(encoder->err_msg, sizeof(encoder->err_msg), lua_tostring(L, -1));
					lua_pop(L, 1);
					return false;
				}
			}
		}
		else {
			snprintf(encoder->err_msg, sizeof(encoder->err_msg), lua_tostring(L, -1));
			lua_pop(L, 1);
			return false;
		}
	}
	else {
		if (is_array) {
			int32_t i;
			for (i=1; i<=size; ++i) {
				lua_rawgeti(L, stack, i);
				if (!encode_value(L, -1, false, encoder, depth+1)) {
					lua_pop(L, 1);
					return false;
				}
				lua_pop(L, 1);
			}
		}
		else {
			lua_pushnil(L);
			while (lua_next(L, stack)) {
				if (!encode_pair(L, encoder, is_array, depth+1)) {
					lua_pop(L, 2);
					return false;
				}
				lua_pop(L, 1);
			}
		}
	}
	if (encoder->write_ptr > MAX_PACKAGE_SIZE) {
		snprintf(encoder->err_msg, sizeof(encoder->err_msg), "Attempt to encode an too large packet, size:%d max:%d", 
			encoder->write_ptr, MAX_PACKAGE_SIZE);
		return false;
	}
	return true;
}

static bool encode_value(lua_State* L, int32_t stack, bool is_key, qseri_encoder* encoder, int32_t depth) {
	int32_t valuet = lua_type(L, stack);
	switch (valuet) {
		case LUA_TNUMBER: {
			if (lua_isinteger(L, stack)) {
				int64_t value = lua_tointeger(L, stack);
				if (value >= 0) {
					write_type_value(encoder, QSERI_TUINT, value);
				}
				else {
					write_byte(encoder, QSERI_TNEGINT);
					write_uinteger(encoder, -value);
				}
			}
			else {
				write_byte(encoder, QSERI_TNUMBER);
				double value = lua_tonumber(L, stack);
				write_buf(encoder, &value, sizeof(value));
			}
			break;
		}
		case LUA_TSTRING: {
			size_t real_len = 0;
			const char* string = luaL_checklstring(L, stack, &real_len);
			size_t len = strlen(string);
			if (encoder->compressible && real_len == len) {
				int32_t offset = string_offset(encoder, lua_tostring(L, stack));
				if (offset < QSERI_TLARGESTRING) {
					write_type_value(encoder, QSERI_TMAGIC, offset);
				}
				else {
					write_byte(encoder, QSERI_TLARGESTRING);
					write_uinteger(encoder, offset);
				}
			}
			else {
				if (real_len == len && real_len < QSERI_TLARGESTRING) {
					write_type_value(encoder, QSERI_TMAGIC, real_len);
				}
				else {
					write_byte(encoder, QSERI_TBUFFER);
					write_uinteger(encoder, real_len);			
				}
				write_buf(encoder, string, real_len);
			}
			break;
		}
		case LUA_TTABLE: {
			int32_t size = 0;
			bool is_array = false;
			if (!table_info(L, stack, &is_array, &size, encoder)) {
				return false;
			}
			if (!encode_table(L, stack, encoder, size, is_array, depth+1)) {
				return false;
			}
			break;
		}
		case LUA_TBOOLEAN: {
			uint8_t b = lua_toboolean(L, stack)?QSERI_TBOOLEANTRUE:QSERI_TBOOLEANFALSE;
			write_byte(encoder, b);
			break;
		}
		case LUA_TNIL: {
			uint8_t b = QSERI_TNIL;
			write_byte(encoder, b);
			break;
		}
		case LUA_TLIGHTUSERDATA: {
			write_byte(encoder, QSERI_TPOINTER);
			void* value = lua_touserdata(L, stack);
			write_buf(encoder, &value, sizeof(value));
			break;
		}
		default: {
			snprintf(encoder->err_msg, sizeof(encoder->err_msg), "Invalid type %s %s", 
				is_key?"key":"value", luaL_typename(L, valuet));
			return false;
			break;
		}
	}
	return true;
}

static bool decode_value(lua_State* L, qseri_qdecoder* decoder);

static bool decode_array(lua_State* L, qseri_qdecoder* decoder, int32_t size) {
	if (size <= 0) {
		lua_newtable(L);
		return true;
	}
	if (size > decoder->size - decoder->read_ptr) {
		return false;
	}
	lua_createtable(L, size, 0);
	int32_t i;
	for (i=1; i<=size; ++i) {
		if (decode_value(L, decoder)) {
			lua_rawseti(L, -2, i);
		}
		else {
			lua_pop(L, 1);
			return false;
		}
	}
	return true;
}

static bool decode_map(lua_State* L, qseri_qdecoder* decoder, int32_t size) {
	if (size <= 0) {
		lua_newtable(L);
		return true;
	}
	if (size > (decoder->size - decoder->read_ptr) / 2) {
		return false;
	}
	lua_createtable(L, 0, size);
	int32_t i;
	for (i=1; i<=size; ++i) {
		if (!decode_value(L, decoder)) {
			lua_pop(L, 1);
			return false;
		}
		if (decode_value(L, decoder)) {
			lua_rawset(L, -3);
		}
		else {
			lua_pop(L, 2);
			return false;
		}
	}
	return true;
}

static bool decode_value(lua_State* L, qseri_qdecoder* decoder) {
	char* buffer = read_buf(decoder, 1);
	if (!buffer) {
		return false;
	}
	uint8_t b = (uint8_t)*buffer;
	uint8_t valuet = b>>6;
	int64_t size = b&0x3F;
	bool small = size != LARGE_TAG;
	switch (valuet) {
		case QSERI_TUINT: {
			if (small) {
				lua_pushinteger(L, size);
			}
			else {
				if (read_uinteger(decoder, &size)) {
					lua_pushinteger(L, size);
				}
				else {
					return false;
				}
			}
			break;
		}
		case QSERI_TMAP: {
			if (!small) {
				if (!read_uinteger(decoder, &size)) {
					return false;
				}
			}
			if (!decode_map(L, decoder, size)) {
				return false;
			}
			break;
		}
		case QSERI_TARRAY: {
			if (!small) {
				if (!read_uinteger(decoder, &size)) {
					return false;
				}
			}
			if (!decode_array(L, decoder, size)) {
				return false;
			}
			break;
		}
		case QSERI_TMAGIC: {
			if (b == QSERI_TNEGINT) {
				int64_t value = 0;
				if (!read_uinteger(decoder, &value)) {
					return false;
				}
				lua_pushinteger(L, -value);
			}
			else if (b == QSERI_TNUMBER) {
				double value = 0;
				buffer = read_buf(decoder, sizeof(value));
				if (!buffer) {
					return false;
				}
				memcpy(&value, buffer, sizeof(value));
				lua_pushnumber(L, value);
			}
			else if (b == QSERI_TLARGESTRING) {
				if (!read_uinteger(decoder, &size)) {
					return false;
				}
				int32_t offset = decoder->string_offsets[size];
				lua_pushstring(L, decoder->buffer+offset);
			}
			else if (b == QSERI_TBUFFER) {
				if (!read_uinteger(decoder, &size)) {
					return false;
				}
				const char* buffer = read_buf(decoder, size);
				if (!buffer) {
					return false;
				}
				lua_pushlstring(L, buffer, size);
			}
			else if (b == QSERI_TBOOLEANTRUE) {
				lua_pushboolean(L, 1);
			}
			else if (b == QSERI_TBOOLEANFALSE) {
				lua_pushboolean(L, 0);
			}
			else if (b == QSERI_TNIL) {
				lua_pushnil(L);
			}
			else if (b == QSERI_TPOINTER) {
				void* value = NULL;
				buffer = read_buf(decoder, sizeof(value));
				if (!buffer) {
					return false;
				}
				memcpy(&value, buffer, sizeof(value));
				lua_pushlightuserdata(L, value);
			}
			else {
				if (size < 0) {
					return false;
				}
				if (decoder->compressible) {
					if (size > decoder->string_offset_count) {
						return false;
					}
					int32_t offset = decoder->string_offsets[size];
					lua_pushstring(L, decoder->buffer+offset);
				}
				else {
					const char* buffer = read_buf(decoder, size);
					if (!buffer) {
						return false;
					}
					lua_pushlstring(L, buffer, size);
				}
			}
			break;
		}
		default:
			break;
	}
	return true;
}

static int32_t strkey_compare(const void* a, const void* b) {
	//use lua string pointer to compare, it's safe and more faster
    return ((const qseri_string_key*)a)->string - ((const qseri_string_key*)b)->string;
}

static inline bool check_compressible(lua_State *L, int32_t stack) {
	stack = lua_absindex(L, stack);
	if (!lua_istable(L, stack)) {
		return false;
	}
	lua_pushnil(L);
	int32_t cnt = 0;
	while (lua_next(L, stack)) {
		if (lua_type(L, -1) != LUA_TTABLE) {
			lua_pop(L, 1);
			continue;
		}
		lua_pushnil(L);
		if (lua_next(L, -2)) {
			if (lua_istable(L, -1)) {
				lua_pop(L, 3);
				return true;
			}
			if (++cnt >= 2) {
				lua_pop(L, 3);
				return true;
			}
			lua_pop(L, 2);
		}
		lua_pop(L, 1);
	}
	return false;
}

void* qseri_pack(lua_State *L, bool* compressible, size_t* size) {
	void* data = NULL;
	*size = 0;

	int32_t top = lua_gettop(L);
	if (top <= 0) {
		data = malloc(1);
		return data;
	}
	if (top > 16) {
		luaL_error(L, "Arg count limit 16, got %d", top);
	}
	qseri_encoder encoder;
	memset(&encoder, 0, sizeof(encoder));
	
	bool success = true;

	//check if compressible
	if (compressible) {
		encoder.compressible = *compressible;
	}
	else {
		int32_t i;
		for(i=1; i<=top; ++i) {
			if (check_compressible(L, i)) {
				encoder.compressible = true;
				break;
			}
		}
	}

	if (encoder.compressible) {
		qbarray_init(&encoder.string_keys, sizeof(qseri_string_key), 16, strkey_compare);
	}
	else {
		assert(!encoder.buffer);
		encoder.cap = BUFFER_INIT_SIZE;
		encoder.buffer = (char*)malloc(encoder.cap);
		++encoder.write_ptr;
	}

	//encode
	int32_t i;
	for(i=1; i<=top; ++i) {
		if (!encode_value(L, i, false, &encoder, 1)) {
			success = false;
			lua_settop(L, top);
			break;
		}
	}

	if (!success) {
		lua_settop(L, top);
		if (strlen(encoder.err_msg) != 0) {
			luaL_error(L, encoder.err_msg);
		}
		else {
			luaL_error(L, "Pack error");
		}
	}

	if (encoder.compressible) {
		int32_t cap = encoder.write_ptr + sizeof(uint32_t) + encoder.string_key_write_ptr + encoder.string_offset_write_ptr*sizeof(uint32_t);
		char* buffer = (char*)malloc(cap);
		int32_t write_ptr = 0;
		buffer[write_ptr++] = 0x80|(char)top;

		//string buffer size
		qbuf_write_uinteger(&buffer, &cap, &write_ptr, encoder.string_key_write_ptr);
		if (encoder.string_key_write_ptr > 0) {
			//string body
			qbuf_write_buf(&buffer, &cap, &write_ptr, encoder.string_key_buffer, encoder.string_key_write_ptr);
		}
		//string offset array size
		qbuf_write_uinteger(&buffer, &cap, &write_ptr, encoder.string_offset_write_ptr);
		int32_t n;
		for (n=0; n<encoder.string_offset_write_ptr; ++n) {
			qbuf_write_uinteger(&buffer, &cap, &write_ptr, encoder.string_offsets[n]);
		}

		//body
		qbuf_write_buf(&buffer, &cap, &write_ptr, encoder.buffer, encoder.write_ptr);
		data = buffer;
		*size = write_ptr;

		qbarray_release(&encoder.string_keys);
		safe_free(encoder.string_key_buffer);
		safe_free(encoder.string_offsets);
		safe_free(encoder.buffer);
	}
	else {
		encoder.buffer[0] = (char)top;
		data = encoder.buffer;
		*size = encoder.write_ptr;
	}

	return data;
}

int32_t qseri_unpack(lua_State *L) {
	void* buffer = NULL;
	size_t size = 0;
	if (lua_isstring(L, 1)) {
		buffer = (void*)luaL_checklstring(L, 1, &size);
	}
	else {
		if (lua_isuserdata(L, 1)) {
			buffer = lua_touserdata(L, 1);
			size = luaL_checkinteger(L, 2);
		}
		else {
			luaL_error(L, "Unpack error, invalid buffer type %s", luaL_typename(L, 1));
		}
	}
	if (size <= 1)
		return 0;
	if (!buffer)
		luaL_error(L, "Attempt to unpack a nil buffer");

	qseri_qdecoder decoder;
	memset(&decoder, 0, sizeof(decoder));
	decoder.buffer = buffer;
	decoder.size = size;
	decoder.read_ptr = 0;

	if (decoder.size <= 0) {
		goto fail;
	}
	int8_t b = decoder.buffer[decoder.read_ptr++];
	decoder.compressible = (b>>7) != 0;
	int32_t argc = b & 0x7F;
	if (decoder.compressible) {
		int64_t string_size = 0;
		if (!read_uinteger(&decoder, &string_size)) {
			goto fail;
		}
		int32_t string_ptr = decoder.read_ptr;
		decoder.read_ptr += string_size;

		int64_t string_offset_count = 0;
		if (!read_uinteger(&decoder, &string_offset_count)) {
			goto fail;
		}
		if (string_offset_count > 0) {
			decoder.string_offset_count = string_offset_count;
			decoder.string_offsets = (int32_t*)malloc(string_offset_count*sizeof(*decoder.string_offsets));
			//fill string offset
			int32_t n;
			for (n=0; n<string_offset_count; ++n) {
				int64_t value = 0;
				if (!read_uinteger(&decoder, &value)) {
					goto fail;
				}
				decoder.string_offsets[n] = value;
			}
		}
		decoder.buffer += string_ptr;
		decoder.read_ptr -= string_ptr;
	}

	int32_t i;
	for(i=0; i<argc; ++i) {
		if (!decode_value(L, &decoder)) {
			goto fail;
		}
	}
	safe_free(decoder.string_offsets);
	return argc;

fail:
	safe_free(decoder.string_offsets);
	if (strlen(decoder.err_msg) != 0) {
		return luaL_error(L, decoder.err_msg);
	}
	else {
		return luaL_error(L, "Unpack error");
	}
}

int32_t lpack(lua_State *L) {
	size_t size = 0;
	void* data = qseri_pack(L, NULL, &size);
	lua_pushlightuserdata(L, data);
	lua_pushinteger(L, size);
	return 2;
}

int32_t lpackstring(lua_State *L) {
	size_t size = 0;
	void* data = qseri_pack(L, NULL, &size);
	lua_pushlstring(L, data, size);
	free(data);
	return 1;
}

int32_t lraw_pack(lua_State *L) {
	bool compressible = false;
	size_t size = 0;
	void* data = qseri_pack(L, &compressible, &size);
	lua_pushlightuserdata(L, data);
	lua_pushinteger(L, size);
	return 2;
}

int32_t lraw_packstring(lua_State *L) {
	bool compressible = false;
	size_t size = 0;
	void* data = qseri_pack(L, &compressible, &size);
	lua_pushlstring(L, data, size);
	free(data);
	return 1;
}

int32_t lcompress_pack(lua_State *L) {
	bool compressible = true;
	size_t size = 0;
	void* data = qseri_pack(L, &compressible, &size);
	lua_pushlightuserdata(L, data);
	lua_pushinteger(L, size);
	return 2;
}

int32_t lcompress_packstring(lua_State *L) {
	bool compressible = true;
	size_t size = 0;
	void* data = qseri_pack(L, &compressible, &size);
	lua_pushlstring(L, data, size);
	free(data);
	return 1;
}

int32_t lunpack(lua_State *L) {
	return qseri_unpack(L);
}

LUAMOD_API int32_t luaopen_qseri(lua_State* L) {
	luaL_Reg l[] = {
		{ "pack", 				lpack },
		{ "packstring", 		lpackstring },
		{ "raw_pack", 			lraw_pack },
		{ "raw_packstring", 	lraw_packstring },
		{ "compress_pack", 		lcompress_pack },
		{ "compress_packstring",lcompress_packstring },
		{ "unpack", 			lunpack },
		{ NULL, NULL },
	};
	luaL_newlib(L, l);
	return 1;
}