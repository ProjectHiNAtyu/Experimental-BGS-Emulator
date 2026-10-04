#include <std_include.hpp>
#include "protobuf.hpp"

#include <cstring>

bool ProtobufWriter::encodeVarInt(uint64_t value)
{
	char tmp[16];
	int  i = 0;

	if (value == 0)
	{
		tmp[0] = 0;
		i      = 1;
	}
	else
	{
		while (value && i < 10)
		{
			tmp[i++] = static_cast<char>((value & 0x7F) | 0x80);
			value  >>= 7;
		}
		tmp[i - 1] &= 0x7F;
	}

	this->buffer.append(tmp, i);
	this->length += i;
	return true;
}

bool ProtobufWriter::encodeVarInt(int64_t value)
{
	uint64_t v = static_cast<uint64_t>(2 * value);
	if (value < 0) v = ~v;
	return this->encodeVarInt(v);
}

bool ProtobufWriter::encodeString(const char* value, uint32_t len)
{
	if (!this->encodeVarInt(static_cast<uint64_t>(len))) return false;
	this->buffer.append(value, len);
	this->length += len;
	return true;
}

bool ProtobufWriter::encodeTag(uint32_t tagId, eWireType wireType)
{
	const uint64_t tag = static_cast<uint64_t>(wireType) | (static_cast<uint64_t>(tagId) << 3);
	return this->encodeVarInt(tag);
}

bool ProtobufWriter::writeInt64(uint32_t tag, int64_t value)
{
	return encodeTag(tag, WIRETYPE_VARINT) && encodeVarInt(value);
}

bool ProtobufWriter::writeUInt64(uint32_t tag, uint64_t value)
{
	return encodeTag(tag, WIRETYPE_VARINT) && encodeVarInt(value);
}

bool ProtobufWriter::writeInt32(uint32_t tag, int32_t value)
{
	return encodeTag(tag, WIRETYPE_VARINT) && encodeVarInt(static_cast<int64_t>(value));
}

bool ProtobufWriter::writeUInt32(uint32_t tag, uint32_t value)
{
	return encodeTag(tag, WIRETYPE_VARINT) && encodeVarInt(static_cast<uint64_t>(value));
}

bool ProtobufWriter::writeString(uint32_t tag, const char* value, uint32_t size)
{
	uint32_t len = size;
	const void* nul = memchr(value, 0, size);
	if (nul) len = static_cast<uint32_t>(static_cast<const char*>(nul) - value);
	return encodeTag(tag, WIRETYPE_STRING) && encodeString(value, len);
}

bool ProtobufWriter::writeBlob(uint32_t tag, const void* buffer, uint32_t size)
{
	return encodeTag(tag, WIRETYPE_STRING)
	    && encodeString(reinterpret_cast<const char*>(buffer), size);
}

bool ProtobufWriter::writeBoolean(uint32_t tag, bool value)
{
	return encodeTag(tag, WIRETYPE_VARINT) && encodeVarInt(value ? 1ULL : 0ULL);
}

bool ProtobufWriter::writeFixed32(uint32_t tag, uint32_t value)
{
	if (!encodeTag(tag, WIRETYPE_32BIT)) return false;
	for (int i = 0; i < 4; ++i)
		this->buffer.push_back(static_cast<char>((value >> (i * 8)) & 0xFF));
	this->length += 4;
	return true;
}

bool ProtobufWriter::writeFixed64(uint32_t tag, uint64_t value)
{
	if (!encodeTag(tag, WIRETYPE_64BIT)) return false;
	for (int i = 0; i < 8; ++i)
		this->buffer.push_back(static_cast<char>((value >> (i * 8)) & 0xFF));
	this->length += 8;
	return true;
}

bool ProtobufWriter::write(const void* value, uint32_t byteCount)
{
	this->buffer.append(static_cast<const char*>(value), byteCount);
	this->length += byteCount;
	return true;
}
