//
// IResearch search engine 
// 
// Copyright (c) 2016 by EMC Corporation, All Rights Reserved
// 
// This software contains the intellectual property of EMC Corporation or is licensed to
// EMC Corporation from third parties. Use of this software and the intellectual property
// contained therein is expressly limited to the terms and conditions of the License
// Agreement under which it is provided by or on behalf of EMC.
// 

#ifndef IRESEARCH_FORMAT_10_ATTRIBUTES
#define IRESEARCH_FORMAT_10_ATTRIBUTES

#include "analysis/token_attributes.hpp"
#include "utils/attributes.hpp"

NS_ROOT

class bitset;

NS_BEGIN(version10)

//////////////////////////////////////////////////////////////////////////////
/// @class documents
/// @brief document set
//////////////////////////////////////////////////////////////////////////////
struct documents : basic_attribute<bitset*> {
  DECLARE_ATTRIBUTE_TYPE();
  DECLARE_FACTORY_DEFAULT();
  documents() NOEXCEPT;
};

struct term_meta : iresearch::term_meta {
  DECLARE_FACTORY_DEFAULT();

  void clear() {
    irs::term_meta::clear();
    doc_start = pos_start = pay_start = 0;
    pos_end = type_limits<type_t::address_t>::invalid();
  }

  uint64_t doc_start = 0; // where this term's postings start in the .doc file
  uint64_t pos_start = 0; // where this term's postings start in the .pos file
  uint64_t pos_end = iresearch::type_limits<iresearch::type_t::address_t>::invalid(); // file pointer where the last (vInt encoded) pos delta is
  uint64_t pay_start = 0; // where this term's payloads/offsets start in the .pay file
  union {
    doc_id_t e_single_doc; // singleton document id delta
    uint64_t e_skip_start; // pointer where skip data starts (after doc_start)
  };
}; // term_meta

NS_END // version10
NS_END // ROOT

#endif // IRESEARCH_FORMAT_10_ATTRIBUTES
