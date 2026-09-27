// XMP metadata, flattened into keys the way exiv2 does it:
//
//   Xmp.dc.format                                       simple text
//   Xmp.dc.subject                                      bag of text items
//   Xmp.dc.title                                        language alternative
//   Xmp.mwg-rs.Regions                                  struct
//   Xmp.mwg-rs.Regions/mwg-rs:RegionList                bag of structs
//   Xmp.mwg-rs.Regions/mwg-rs:RegionList[1]/mwg-rs:Name a field of item 1
//
// A key is "Xmp.<prefix>.<path>". The first path step is the property's local
// name; later steps are "prefix:name", with "[n]" (1-based) selecting an item
// of an array. Prefixes are those of the namespace registry below, not
// necessarily the document's. Qualifiers other than xml:lang are not kept.
#pragma once

#include <photos/export.hpp>
#include <photos/types.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace photos {

class PHOTOS_EXPORT XmpValue {
 public:
  enum class Kind {
    text,       // a simple property
    bag,        // unordered array
    seq,        // ordered array
    alt,        // alternative array
    langAlt,    // alternative array of language-tagged text
    structure,  // a struct; its fields are separate datums
  };

  XmpValue() = default;
  explicit XmpValue(Kind kind) : kind_(kind) {}
  static XmpValue text(std::string value);
  static XmpValue array(Kind kind, std::vector<std::string> items);
  static XmpValue langAlt(std::string xDefault);

  Kind kind() const noexcept { return kind_; }
  bool isArray() const noexcept { return kind_ == Kind::bag || kind_ == Kind::seq || kind_ == Kind::alt; }

  // The text of a simple value.
  const std::string& text() const noexcept { return text_; }
  // The items of an array. An array of structs has no items here: each item
  // is a datum of its own, "<key>[n]".
  const std::vector<std::string>& items() const noexcept { return items_; }
  std::vector<std::string>& items() noexcept { return items_; }
  // Language alternatives as (language, text), "x-default" first.
  const std::vector<std::pair<std::string, std::string>>& languages() const noexcept { return langs_; }
  // The text for the language, or for x-default, or the first.
  std::optional<std::string> langText(std::string_view lang = "x-default") const;
  void setLangText(std::string_view lang, std::string text);

  // As exiv2 prints it: text as is, array items joined with ", ", language
  // alternatives as lang="x-default" text, ...
  std::string toString() const;

  friend PHOTOS_EXPORT bool operator==(const XmpValue& a, const XmpValue& b) noexcept;
  friend bool operator!=(const XmpValue& a, const XmpValue& b) noexcept { return !(a == b); }

 private:
  Kind kind_ = Kind::text;
  std::string text_;
  std::vector<std::string> items_;
  std::vector<std::pair<std::string, std::string>> langs_;
};

PHOTOS_EXPORT const char* toString(XmpValue::Kind kind) noexcept;

class PHOTOS_EXPORT XmpDatum {
 public:
  // Throws Error(invalidArgument) for a malformed key or unknown prefix.
  XmpDatum(std::string key, XmpValue value);

  const std::string& key() const noexcept { return key_; }
  // "dc" for Xmp.dc.subject.
  std::string prefix() const;
  // "subject" for Xmp.dc.subject; the whole path for a nested key.
  std::string name() const;
  std::string namespaceUri() const;

  const XmpValue& value() const noexcept { return value_; }
  XmpValue& value() noexcept { return value_; }
  void setValue(XmpValue value) { value_ = std::move(value); }
  XmpValue::Kind kind() const noexcept { return value_.kind(); }
  std::string toString() const { return value_.toString(); }

  // Assigning text keeps the datum's kind: a simple value becomes the text,
  // an array becomes that one item, a language alternative gets it as
  // x-default.
  XmpDatum& operator=(std::string text);
  XmpDatum& operator=(const char* text) { return *this = std::string(text); }
  XmpDatum& operator=(std::string_view text) { return *this = std::string(text); }
  XmpDatum& operator=(const std::vector<std::string>& items);
  XmpDatum& operator=(XmpValue value) {
    value_ = std::move(value);
    return *this;
  }

 private:
  std::string key_;
  XmpValue value_;
};

struct XmpWriteOptions {
  // Whitespace after the packet so others can edit it in place.
  std::size_t padding = 2048;
  // Omit the <?xpacket?> wrapper (sidecar files keep it by default too).
  bool omitPacketWrapper = false;
};

class PHOTOS_EXPORT XmpData {
 public:
  using iterator = std::vector<XmpDatum>::iterator;
  using const_iterator = std::vector<XmpDatum>::const_iterator;

  // The datum for the key, added if there is none. A new datum's kind comes
  // from the known-property table (Xmp.dc.subject is a bag, Xmp.dc.title a
  // language alternative, ...), else text.
  XmpDatum& operator[](std::string_view key);
  void add(XmpDatum datum);
  void add(std::string key, XmpValue value) { add(XmpDatum(std::move(key), std::move(value))); }

  XmpDatum* find(std::string_view key);
  const XmpDatum* find(std::string_view key) const;
  iterator erase(iterator pos) { return data_.erase(pos); }
  // Removes the key and everything nested under it ("<key>/...", "<key>[n]").
  std::size_t erase(std::string_view key);
  void clear() noexcept { data_.clear(); }
  void sortByKey();

  bool empty() const noexcept { return data_.empty(); }
  std::size_t size() const noexcept { return data_.size(); }
  iterator begin() noexcept { return data_.begin(); }
  iterator end() noexcept { return data_.end(); }
  const_iterator begin() const noexcept { return data_.begin(); }
  const_iterator end() const noexcept { return data_.end(); }

  // Parses an XMP packet (UTF-8 RDF/XML). Throws Error(corruptData).
  static XmpData parse(std::string_view packet);
  // Serialises to an XMP packet. Throws Error(invalidArgument) for keys that
  // cannot be laid out (an item index with no array, and the like).
  std::string serialize(const XmpWriteOptions& options = {}) const;

 private:
  std::vector<XmpDatum> data_;
};

// The namespace registry maps prefixes to URIs. It starts with the common
// photo namespaces (dc, xmp, xmpRights, xmpMM, photoshop, tiff, exif, exifEX,
// aux, crs, lr, Iptc4xmpCore, Iptc4xmpExt, mwg-rs, stArea, stDim, ...).
// Namespaces found in parsed packets are added under the document's prefix
// (or a numbered variant when that prefix is taken). Thread-safe.
PHOTOS_EXPORT void registerXmpNamespace(const std::string& uri, const std::string& prefix);
PHOTOS_EXPORT std::optional<std::string> xmpNamespaceUri(std::string_view prefix);
PHOTOS_EXPORT std::optional<std::string> xmpNamespacePrefix(std::string_view uri);

}  // namespace photos
