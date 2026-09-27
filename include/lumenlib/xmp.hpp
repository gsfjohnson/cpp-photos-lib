// XMP metadata, flattened into one entry per property, keyed by the path
// syntax of the XMP specification (part 2) and the XMP Toolkit:
//
//   dc:format                                          simple text
//   dc:subject                                         bag of text items
//   dc:title                                           language alternative
//   mwg-rs:Regions                                     struct
//   mwg-rs:Regions/mwg-rs:RegionList                   bag of structs
//   mwg-rs:Regions/mwg-rs:RegionList[1]/mwg-rs:Name    a field of item 1
//
// Every step is "prefix:name", with "[n]" (1-based) selecting an item of an
// array. Prefixes are those of the namespace registry below (the schemas'
// own preferred prefixes: Iptc4xmpCore, Iptc4xmpExt, exifEX, ...), not
// necessarily the document's. Qualifiers other than xml:lang are not kept.
#pragma once

#include <lumenlib/export.hpp>
#include <lumenlib/types.hpp>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lumenlib {

class LUMENLIB_EXPORT XmpValue {
 public:
  enum class Kind {
    text,       // a simple property
    bag,        // unordered array
    seq,        // ordered array
    alt,        // alternative array
    langAlt,    // alternative array of language-tagged text
    structure,  // a struct; its fields are entries of their own
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
  // is an entry of its own, "<path>[n]".
  const std::vector<std::string>& items() const noexcept { return items_; }
  std::vector<std::string>& items() noexcept { return items_; }
  // Language alternatives as (language, text), "x-default" first.
  const std::vector<std::pair<std::string, std::string>>& languages() const noexcept { return langs_; }
  // The text for the language, or for x-default, or the first.
  std::optional<std::string> langText(std::string_view lang = "x-default") const;
  void setLangText(std::string_view lang, std::string text);

  // One line of text: a simple value as is, array items joined with ", ",
  // language alternatives as `lang="x-default" text, ...`.
  std::string summary() const;

  friend LUMENLIB_EXPORT bool operator==(const XmpValue& a, const XmpValue& b) noexcept;
  friend bool operator!=(const XmpValue& a, const XmpValue& b) noexcept { return !(a == b); }

 private:
  Kind kind_ = Kind::text;
  std::string text_;
  std::vector<std::string> items_;
  std::vector<std::pair<std::string, std::string>> langs_;
};

// "text", "bag", "seq", "alt", "lang-alt" or "struct".
LUMENLIB_EXPORT const char* xmpKindName(XmpValue::Kind kind) noexcept;

class LUMENLIB_EXPORT XmpEntry {
 public:
  // Throws Error(invalidArgument) for a malformed path or unknown prefix.
  XmpEntry(std::string path, XmpValue value);

  const std::string& path() const noexcept { return path_; }
  // "dc" for dc:subject: the prefix of the first step.
  std::string prefix() const;
  // "subject" for dc:subject: the name of the first step.
  std::string name() const;
  std::string namespaceUri() const;
  // Whether the path is the property itself or lies under it ("[n]" or "/").
  bool isUnder(std::string_view property) const noexcept;

  const XmpValue& value() const noexcept { return value_; }
  XmpValue& value() noexcept { return value_; }
  void setValue(XmpValue value) { value_ = std::move(value); }
  XmpValue::Kind kind() const noexcept { return value_.kind(); }
  std::string summary() const { return value_.summary(); }

  // Text keeps the entry's kind: a simple value becomes the text, an array
  // that one item, a language alternative gets it as x-default.
  void setText(std::string text);
  // The items of an array (a bag unless the entry is already an array).
  void setItems(std::vector<std::string> items);

 private:
  std::string path_;
  XmpValue value_;
};

struct XmpWriteOptions {
  // Whitespace after the packet so others can edit it in place.
  std::size_t padding = 2048;
  // The <?xpacket?> processing instructions around the packet.
  bool packetWrapper = true;
  // Simple top-level properties as attributes of rdf:Description (the
  // compact form sidecar files often use) rather than elements.
  bool compact = false;
};

class LUMENLIB_EXPORT XmpMetadata {
 public:
  using iterator = std::vector<XmpEntry>::iterator;
  using const_iterator = std::vector<XmpEntry>::const_iterator;

  XmpEntry* find(std::string_view path);
  const XmpEntry* find(std::string_view path) const;
  bool contains(std::string_view path) const { return find(path) != nullptr; }
  // The text of a simple property, the x-default (or first) language of a
  // language alternative, or the first item of an array; std::nullopt when
  // absent or a struct.
  std::optional<std::string> text(std::string_view path) const;

  // The entry for the path, added if there is none. A new entry's kind comes
  // from the known-property table (dc:subject is a bag, dc:title a language
  // alternative, ...), else text. Throws Error(invalidArgument) for a bad
  // path.
  XmpEntry& entry(std::string_view path);
  // Replaces the property's value (and anything nested under it).
  XmpEntry& set(std::string_view path, XmpValue value);
  // As XmpEntry::setText and setItems on the path's entry.
  XmpEntry& setText(std::string_view path, std::string text);
  XmpEntry& setItems(std::string_view path, std::vector<std::string> items);
  // Sets one language of a language alternative, keeping the others (a
  // property of another kind is replaced). Empty text removes that language,
  // and the property with the last one.
  void setLangText(std::string_view path, std::string_view lang, std::string text);
  // Adds an entry, even when the path is already there.
  void append(XmpEntry entry);

  iterator erase(iterator pos) { return entries_.erase(pos); }
  // Removes the property and everything nested under it ("<path>/...",
  // "<path>[n]"); returns how many entries went.
  std::size_t remove(std::string_view path);
  template <typename Predicate>
  std::size_t removeIf(Predicate predicate) {
    const auto before = entries_.size();
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(), predicate), entries_.end());
    return before - entries_.size();
  }
  void clear() noexcept { entries_.clear(); }
  void sort();

  bool empty() const noexcept { return entries_.empty(); }
  std::size_t size() const noexcept { return entries_.size(); }
  iterator begin() noexcept { return entries_.begin(); }
  iterator end() noexcept { return entries_.end(); }
  const_iterator begin() const noexcept { return entries_.begin(); }
  const_iterator end() const noexcept { return entries_.end(); }

  // Parses an XMP packet (RDF/XML in UTF-8, or UTF-16/32 with or without a
  // byte-order mark). Throws Error(corruptData).
  static XmpMetadata parse(std::string_view packet);
  // Serialises to an XMP packet in UTF-8. Throws Error(invalidArgument) for
  // paths that cannot be laid out (an item index with no array, and the
  // like).
  std::string serialize(const XmpWriteOptions& options = {}) const;

 private:
  std::vector<XmpEntry> entries_;
};

// The namespace registry maps prefixes to URIs. It starts with the common
// photo schemas under their preferred prefixes (dc, xmp, xmpRights, xmpMM,
// photoshop, tiff, exif, exifEX, aux, crs, lr, Iptc4xmpCore, Iptc4xmpExt,
// plus, mwg-rs, stArea, stDim, ...). Namespaces found in parsed packets are
// added under the document's prefix (or a numbered variant when that prefix
// is taken). Thread-safe.
LUMENLIB_EXPORT void registerXmpNamespace(const std::string& uri, const std::string& prefix);
LUMENLIB_EXPORT std::optional<std::string> xmpNamespaceUri(std::string_view prefix);
LUMENLIB_EXPORT std::optional<std::string> xmpNamespacePrefix(std::string_view uri);

}  // namespace lumenlib
