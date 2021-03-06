//===- BinaryStreamWriter.cpp - Writes objects to a BinaryStream ----------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "llvm/DebugInfo/MSF/BinaryStreamWriter.h"

#include "llvm/DebugInfo/MSF/BinaryStreamReader.h"
#include "llvm/DebugInfo/MSF/BinaryStreamRef.h"

using namespace llvm;

BinaryStreamWriter::BinaryStreamWriter(WritableBinaryStreamRef S)
    : Stream(S), Offset(0) {}

Error BinaryStreamWriter::writeBytes(ArrayRef<uint8_t> Buffer) {
  if (auto EC = Stream.writeBytes(Offset, Buffer))
    return EC;
  Offset += Buffer.size();
  return Error::success();
}

Error BinaryStreamWriter::writeInteger(uint64_t Value, uint32_t ByteSize) {
  assert(ByteSize == 1 || ByteSize == 2 || ByteSize == 4 || ByteSize == 8);
  uint8_t Bytes[8];
  MutableArrayRef<uint8_t> Buffer(Bytes);
  Buffer = Buffer.take_front(ByteSize);
  switch (ByteSize) {
  case 1:
    Buffer[0] = static_cast<uint8_t>(Value);
    break;
  case 2:
    llvm::support::endian::write16(Buffer.data(), Value, Stream.getEndian());
    break;
  case 4:
    llvm::support::endian::write32(Buffer.data(), Value, Stream.getEndian());
    break;
  case 8:
    llvm::support::endian::write64(Buffer.data(), Value, Stream.getEndian());
    break;
  }
  return writeBytes(Buffer);
}

Error BinaryStreamWriter::writeCString(StringRef Str) {
  if (auto EC = writeFixedString(Str))
    return EC;
  if (auto EC = writeObject('\0'))
    return EC;

  return Error::success();
}

Error BinaryStreamWriter::writeFixedString(StringRef Str) {
  return writeBytes(ArrayRef<uint8_t>(Str.bytes_begin(), Str.bytes_end()));
}

Error BinaryStreamWriter::writeStreamRef(BinaryStreamRef Ref) {
  return writeStreamRef(Ref, Ref.getLength());
}

Error BinaryStreamWriter::writeStreamRef(BinaryStreamRef Ref, uint32_t Length) {
  BinaryStreamReader SrcReader(Ref.slice(0, Length));
  // This is a bit tricky.  If we just call readBytes, we are requiring that it
  // return us the entire stream as a contiguous buffer.  There is no guarantee
  // this can be satisfied by returning a reference straight from the buffer, as
  // an implementation may not store all data in a single contiguous buffer.  So
  // we iterate over each contiguous chunk, writing each one in succession.
  while (SrcReader.bytesRemaining() > 0) {
    ArrayRef<uint8_t> Chunk;
    if (auto EC = SrcReader.readLongestContiguousChunk(Chunk))
      return EC;
    if (auto EC = writeBytes(Chunk))
      return EC;
  }
  return Error::success();
}
