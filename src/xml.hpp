// Internal: a small non-validating XML parser with namespace support, enough
// for XMP. Document type declarations are skipped, never processed, so entity
// expansion attacks do not apply; nesting depth is limited.
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lumenlib::detail {

struct XmlAttribute {
  std::string uri;
  std::string local;
  std::string prefix;
  std::string value;
};

struct XmlElement {
  std::string uri;
  std::string local;
  std::string prefix;
  std::vector<XmlAttribute> attributes;
  std::vector<std::unique_ptr<XmlElement>> children;
  // The character data directly inside the element, concatenated.
  std::string text;
  // Namespace declarations on this element: (prefix, uri).
  std::vector<std::pair<std::string, std::string>> declarations;

  const XmlAttribute* attribute(std::string_view ns, std::string_view name) const {
    for (const auto& a : attributes) {
      if (a.uri == ns && a.local == name) return &a;
    }
    return nullptr;
  }
};

constexpr const char* kXmlNamespace = "http://www.w3.org/XML/1998/namespace";

// Parses a document and returns its root element. Throws Error(corruptData).
std::unique_ptr<XmlElement> parseXml(std::string_view document);

// Escapes text for element content or attribute values.
std::string escapeXml(std::string_view text);

}  // namespace lumenlib::detail
