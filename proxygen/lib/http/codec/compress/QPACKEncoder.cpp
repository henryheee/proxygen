/*
 *  Copyright (c) 2015-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include <proxygen/lib/http/codec/compress/QPACKEncoder.h>

using std::vector;

namespace proxygen {

QPACKEncoder::QPACKEncoder(bool huffman, uint32_t tableSize) :
    // We only need the 'QPACK' table if we are using base index
    HPACKEncoderBase(huffman),
    QPACKContext(tableSize, true),
    controlBuffer_(kBufferGrowth, huffman) {
  // Default the encoder indexing strategy; it can be updated later as well
  setHeaderIndexingStrategy(HeaderIndexingStrategy::getDefaultInstance());
}

QPACKEncoder::EncodeResult
QPACKEncoder::encode(const vector<HPACKHeader>& headers,
                     uint32_t headroom,
                     uint64_t streamId) {
  if (headroom) {
    streamBuffer_.addHeadroom(headroom);
  }
  handlePendingContextUpdate(controlBuffer_, table_.capacity());
  return encodeQ(headers, streamId);
}

QPACKEncoder::EncodeResult
QPACKEncoder::encodeQ(const vector<HPACKHeader>& headers, uint64_t streamId) {
  auto& blockRefList = outstanding_[streamId];
  blockRefList.emplace_back();
  blockReferences_ = &blockRefList.back();
  auto baseIndex = table_.getBaseIndex();

  uint32_t largestReference = 0;
  for (const auto& header: headers) {
    encodeHeaderQ(header, baseIndex, &largestReference);
  }

  auto streamBlock = streamBuffer_.release();

  // encode the prefix
  streamBuffer_.encodeInteger(largestReference);
  if (largestReference > baseIndex) {
    streamBuffer_.encodeInteger(largestReference - baseIndex,
                                HPACK::Q_DELTA_BASE_NEG,
                                HPACK::Q_DELTA_BASE.prefixLength);
  } else {
    streamBuffer_.encodeInteger(baseIndex - largestReference,
                                HPACK::Q_DELTA_BASE_POS,
                                HPACK::Q_DELTA_BASE.prefixLength);
  }
  auto streamBuffer = streamBuffer_.release();
  streamBuffer->prependChain(std::move(streamBlock));

  auto controlBuf = controlBuffer_.release();
  if (controlBuf && controlBuf->computeChainDataLength() > 0) {
    VLOG(5) << "Pushing outstandingControl_ largestReference="
            << largestReference;
    outstandingControl_.push_back(table_.getBaseIndex());
  }
  // blockReferences_ could be empty, if the block encodes only static
  // headers and/or literals

  return { std::move(controlBuf), std::move(streamBuffer) };
}

void QPACKEncoder::encodeHeaderQ(
  const HPACKHeader& header, uint32_t baseIndex, uint32_t* largestReference) {
  uint32_t index = getStaticTable().getIndex(header);
  if (index > 0) {
    // static reference
    streamBuffer_.encodeInteger(index,
                                HPACK::Q_INDEXED.code | HPACK::Q_INDEXED_STATIC,
                                HPACK::Q_INDEXED.prefixLength);
    return;
  }

  bool indexable = shouldIndex(header);
  if (indexable) {
    index = table_.getIndex(header, allowVulnerable_);
    if (index == QPACKHeaderTable::UNACKED) {
      index = 0;
      indexable = false;
    }
  }
  if (index != 0) {
    // dynamic reference
    bool duplicated = false;
    std::tie(duplicated, index) = maybeDuplicate(index);
    indexable &= (duplicated && index == 0);
  }
  if (index == 0) {
    // No valid entry matching header, see if there's a matching name
    uint32_t nameIndex = 0;
    uint32_t absoluteNameIndex = 0;
    bool isStaticName = false;
    std::tie(isStaticName, nameIndex, absoluteNameIndex) =
      getNameIndexQ(header.name);

    // Now check if we should emit an insertion on the control stream
    if (indexable && table_.canIndex(header)) {
      encodeInsertQ(header, isStaticName, nameIndex);
      CHECK(table_.add(header));
      if (allowVulnerable_) {
        index = table_.getBaseIndex();
      } else {
        index = 0;
      }
    }

    if (index == 0) {
      // Couldn't insert it: table full, not indexable, or table contains
      // vulnerable reference.  Encode a literal on the request stream.
      encodeStreamLiteralQ(header, isStaticName, nameIndex, absoluteNameIndex,
                           baseIndex, largestReference);
      return;
    }
  }

  // Encoding a dynamic index reference
  DCHECK_NE(index, 0);
  trackReference(index, largestReference);
  if (index > baseIndex) {
    streamBuffer_.encodeInteger(index - baseIndex - 1, HPACK::Q_INDEXED_POST);
  } else {
    streamBuffer_.encodeInteger(baseIndex - index, HPACK::Q_INDEXED);
  }
}

bool QPACKEncoder::shouldIndex(const HPACKHeader& header) const {
  return (header.bytes() <= table_.capacity()) &&
    (!indexingStrat_ || indexingStrat_->indexHeader(header));
}

std::pair<bool, uint32_t> QPACKEncoder::maybeDuplicate(
      uint32_t relativeIndex) {
  auto res = table_.maybeDuplicate(relativeIndex, allowVulnerable_);
  if (res.first) {
    VLOG(4) << "Encoded duplicate index=" << relativeIndex;
    encodeDuplicate(relativeIndex);
  }
  return res;
}

std::tuple<bool, uint32_t, uint32_t> QPACKEncoder::getNameIndexQ(
  const HPACKHeaderName& headerName) {
  uint32_t absoluteNameIndex = 0;
  uint32_t nameIndex = getStaticTable().nameIndex(headerName);
  bool isStatic = true;
  if (nameIndex == 0) {
    // check dynamic table
    nameIndex = table_.nameIndex(headerName, allowVulnerable_);
    if (nameIndex != 0) {
      absoluteNameIndex = maybeDuplicate(nameIndex).second;
      if (absoluteNameIndex) {
        isStatic = false;
        nameIndex = table_.absoluteToRelative(absoluteNameIndex);
      } else {
        nameIndex = 0;
        absoluteNameIndex = 0;
      }
    }
  }
  return std::tuple<bool, uint32_t, uint32_t>(
    isStatic, nameIndex, absoluteNameIndex);
}

void QPACKEncoder::encodeStreamLiteralQ(
  const HPACKHeader& header, bool isStaticName, uint32_t nameIndex,
  uint32_t absoluteNameIndex, uint32_t baseIndex, uint32_t* largestReference) {
  if (absoluteNameIndex > 0) {
    // Dynamic name reference
    if (absoluteNameIndex > baseIndex && !allowVulnerable_) {
      // Disallowed vulnerable reference
      absoluteNameIndex = 0;
    } else {
      trackReference(absoluteNameIndex, largestReference);
    }
  }
  if (absoluteNameIndex > baseIndex) {
    encodeLiteralQ(header,
                   false, /* not static */
                   true, /* post base */
                   absoluteNameIndex - baseIndex,
                   HPACK::Q_LITERAL_NAME_REF_POST);
  } else {
    encodeLiteralQ(header,
                   isStaticName,
                   false, /* not post base */
                   isStaticName ? nameIndex : baseIndex - absoluteNameIndex + 1,
                   HPACK::Q_LITERAL_NAME_REF);
  }
}

void QPACKEncoder::trackReference(uint32_t absoluteIndex,
                                  uint32_t* largestReference) {
  CHECK_NE(absoluteIndex, 0);
  if (absoluteIndex > *largestReference) {
    *largestReference = absoluteIndex;
  }
  CHECK(blockReferences_);
  auto res = blockReferences_->insert(absoluteIndex);
  if (res.second) {
    VLOG(5) << "Bumping refcount for absoluteIndex=" << absoluteIndex;
    table_.addRef(absoluteIndex);
  }
}

void QPACKEncoder::encodeDuplicate(uint32_t index) {
  DCHECK_GT(index, 0);
  controlBuffer_.encodeInteger(index - 1, HPACK::Q_DUPLICATE);
}

void QPACKEncoder::encodeInsertQ(const HPACKHeader& header,
                                 bool isStaticName,
                                 uint32_t nameIndex) {
  encodeLiteralQHelper(
    controlBuffer_, header, isStaticName, nameIndex,
    HPACK::Q_INSERT_NAME_REF_STATIC, HPACK::Q_INSERT_NAME_REF,
    HPACK::Q_INSERT_NO_NAME_REF);
}

void QPACKEncoder::encodeLiteralQ(const HPACKHeader& header,
                                  bool isStaticName,
                                  bool postBase,
                                  uint32_t nameIndex,
                                  const HPACK::Instruction& idxInstr) {
  DCHECK(!isStaticName || !postBase);
  encodeLiteralQHelper(
      streamBuffer_, header, isStaticName, nameIndex,
      HPACK::Q_LITERAL_STATIC, idxInstr,
      HPACK::Q_LITERAL);
}

void QPACKEncoder::encodeLiteralQHelper(HPACKEncodeBuffer& buffer,
                                        const HPACKHeader& header,
                                        bool isStaticName,
                                        uint32_t nameIndex,
                                        uint8_t staticFlag,
                                        const HPACK::Instruction& idxInstr,
                                        const HPACK::Instruction& litInstr) {
  // name
  if (nameIndex) {
    VLOG(10) << "encoding name index=" << nameIndex;
    DCHECK_NE(nameIndex, QPACKHeaderTable::UNACKED);
    uint8_t byte = idxInstr.code;
    if (isStaticName) {
      byte |= staticFlag;
    } else {
      DCHECK_GE(nameIndex, 1);
      nameIndex -= 1;
    }
    buffer.encodeInteger(nameIndex, byte, idxInstr.prefixLength);
  } else {
    buffer.encodeLiteral(litInstr.code, litInstr.prefixLength,
                         header.name.get());
  }
  // value
  buffer.encodeLiteral(header.value);
}

HPACK::DecodeError QPACKEncoder::onControlHeaderAck() {
  if (outstandingControl_.empty()) {
    LOG(ERROR) << "Received control stream ack with no outstanding blocks";
    return HPACK::DecodeError::INVALID_ACK;
  }
  VLOG(5) << "onControlHeaderAck, maxAcked=" << outstandingControl_.front();
  table_.setMaxAcked(outstandingControl_.front());
  outstandingControl_.pop_front();
  return HPACK::DecodeError::NONE;
}

HPACK::DecodeError QPACKEncoder::onHeaderAck(uint64_t streamId, bool all) {
  auto it = outstanding_.find(streamId);
  if (it == outstanding_.end()) {
    LOG(ERROR) << "Received an ack with no outstanding header blocks stream="
               << streamId;
    return HPACK::DecodeError::INVALID_ACK;
  }
  VLOG(5) << "onHeaderAck streamId=" << streamId;
  if (all) {
    // Happens when a stream is reset
    for (auto& references: it->second) {
      for (auto i: references) {
        table_.subRef(i);
      }
    }
    it->second.clear();
  } else {
    auto references = std::move(it->second.front());
    it->second.pop_front();
    // a different stream, sub all the references
    for (auto i: references) {
      VLOG(5) << "Decrementing refcount for absoluteIndex=" << i;
      table_.subRef(i);
    }
  }
  if (it->second.empty()) {
    outstanding_.erase(it);
  }
  return HPACK::DecodeError::NONE;
}

}
