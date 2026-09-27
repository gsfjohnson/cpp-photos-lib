#include "xml.hpp"

#include "bytes.hpp"

#include <map>

namespace photos::detail {
namespace {

constexpr int kMaxDepth = 256;

bool isNameStart(unsigned char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == ':' || c >= 0x80;
}
bool isNameChar(unsigned char c) { return isNameStart(c) || (c >= '0' && c <= '9') || c == '-' || c == '.'; }
bool isWs(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

void appendUtf8(std::string& out, std::uint32_t cp) {
  if (cp == 0 || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) corrupt("invalid XML character reference");
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xc0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3f));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xe0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
    out += static_cast<char>(0x80 | (cp & 0x3f));
  } else {
    out += static_cast<char>(0xf0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
    out += static_cast<char>(0x80 | (cp & 0x3f));
  }
}

class Parser {
 public:
  explicit Parser(std::string_view doc) : s_(doc) {}

  std::unique_ptr<XmlElement> document() {
    if (s_.size() >= 3 && static_cast<unsigned char>(s_[0]) == 0xef && static_cast<unsigned char>(s_[1]) == 0xbb &&
        static_cast<unsigned char>(s_[2]) == 0xbf) {
      pos_ = 3;
    }
    if (s_.size() >= 2 && ((static_cast<unsigned char>(s_[0]) == 0xfe && static_cast<unsigned char>(s_[1]) == 0xff) ||
                           (static_cast<unsigned char>(s_[0]) == 0xff && static_cast<unsigned char>(s_[1]) == 0xfe))) {
      corrupt("UTF-16 XML is not supported");
    }
    for (;;) {
      skipWs();
      if (pos_ >= s_.size()) corrupt("XML document has no root element");
      if (lookingAt("<?")) {
        skipPast("?>");
      } else if (lookingAt("<!--")) {
        skipPast("-->");
      } else if (lookingAt("<!DOCTYPE")) {
        skipDoctype();
      } else if (lookingAt("<")) {
        break;
      } else {
        corrupt("text before the XML root element");
      }
    }
    std::vector<std::map<std::string, std::string>> scopes;
    scopes.push_back({{"xml", kXmlNamespace}});
    // What follows the root (padding, <?xpacket end?>) is ignored.
    return element(scopes, 0);
  }

 private:
  bool lookingAt(std::string_view t) const { return s_.substr(pos_, t.size()) == t; }

  void skipWs() {
    while (pos_ < s_.size() && isWs(s_[pos_])) ++pos_;
  }

  void skipPast(std::string_view end) {
    const auto p = s_.find(end, pos_);
    if (p == std::string_view::npos) corrupt("unterminated XML construct");
    pos_ = p + end.size();
  }

  void skipDoctype() {
    int depth = 0;
    for (; pos_ < s_.size(); ++pos_) {
      const char c = s_[pos_];
      if (c == '[')
        ++depth;
      else if (c == ']')
        --depth;
      else if (c == '>' && depth <= 0) {
        ++pos_;
        return;
      }
    }
    corrupt("unterminated DOCTYPE");
  }

  std::string name() {
    const std::size_t start = pos_;
    if (pos_ >= s_.size() || !isNameStart(static_cast<unsigned char>(s_[pos_]))) corrupt("expected an XML name");
    while (pos_ < s_.size() && isNameChar(static_cast<unsigned char>(s_[pos_]))) ++pos_;
    return std::string(s_.substr(start, pos_ - start));
  }

  // Decodes an entity or character reference at pos_ ('&').
  void entity(std::string& out) {
    const auto semi = s_.find(';', pos_);
    if (semi == std::string_view::npos || semi - pos_ > 12) corrupt("bad XML entity");
    const auto ref = s_.substr(pos_ + 1, semi - pos_ - 1);
    pos_ = semi + 1;
    if (ref == "lt")
      out += '<';
    else if (ref == "gt")
      out += '>';
    else if (ref == "amp")
      out += '&';
    else if (ref == "quot")
      out += '"';
    else if (ref == "apos")
      out += '\'';
    else if (ref.size() > 1 && ref[0] == '#') {
      std::uint32_t cp = 0;
      const bool hex = ref[1] == 'x' || ref[1] == 'X';
      const auto digits = ref.substr(hex ? 2 : 1);
      if (digits.empty()) corrupt("bad XML character reference");
      for (char c : digits) {
        std::uint32_t d;
        if (c >= '0' && c <= '9')
          d = static_cast<std::uint32_t>(c - '0');
        else if (hex && c >= 'a' && c <= 'f')
          d = static_cast<std::uint32_t>(c - 'a' + 10);
        else if (hex && c >= 'A' && c <= 'F')
          d = static_cast<std::uint32_t>(c - 'A' + 10);
        else
          corrupt("bad XML character reference");
        cp = cp * (hex ? 16 : 10) + d;
        if (cp > 0x10ffff) corrupt("bad XML character reference");
      }
      appendUtf8(out, cp);
    } else {
      corrupt("unknown XML entity &" + std::string(ref) + ";");
    }
  }

  std::string attributeValue() {
    if (pos_ >= s_.size() || (s_[pos_] != '"' && s_[pos_] != '\'')) corrupt("expected a quoted attribute value");
    const char quote = s_[pos_++];
    std::string out;
    while (pos_ < s_.size() && s_[pos_] != quote) {
      if (s_[pos_] == '&') {
        entity(out);
      } else if (s_[pos_] == '<') {
        corrupt("'<' in an attribute value");
      } else {
        const char c = s_[pos_++];
        out += (c == '\t' || c == '\n' || c == '\r') ? ' ' : c;
      }
    }
    if (pos_ >= s_.size()) corrupt("unterminated attribute value");
    ++pos_;
    return out;
  }

  static void splitName(const std::string& qname, std::string& prefix, std::string& local) {
    const auto colon = qname.find(':');
    if (colon == std::string::npos) {
      prefix.clear();
      local = qname;
    } else {
      prefix = qname.substr(0, colon);
      local = qname.substr(colon + 1);
    }
  }

  static const std::string* lookup(const std::vector<std::map<std::string, std::string>>& scopes,
                                   const std::string& prefix) {
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
      const auto f = it->find(prefix);
      if (f != it->end()) return &f->second;
    }
    return nullptr;
  }

  std::unique_ptr<XmlElement> element(std::vector<std::map<std::string, std::string>>& scopes, int depth) {
    if (depth > kMaxDepth) corrupt("XML nested too deeply");
    ++pos_;  // '<'
    auto el = std::make_unique<XmlElement>();
    const std::string qname = name();
    struct RawAttr {
      std::string qname, value;
    };
    std::vector<RawAttr> raw;
    std::map<std::string, std::string> scope;
    bool empty = false;
    for (;;) {
      skipWs();
      if (pos_ >= s_.size()) corrupt("unterminated start tag");
      if (lookingAt("/>")) {
        pos_ += 2;
        empty = true;
        break;
      }
      if (s_[pos_] == '>') {
        ++pos_;
        break;
      }
      std::string an = name();
      skipWs();
      if (pos_ >= s_.size() || s_[pos_] != '=') corrupt("expected '=' after attribute name");
      ++pos_;
      skipWs();
      std::string av = attributeValue();
      if (an == "xmlns") {
        scope[""] = av;
        el->declarations.emplace_back("", av);
      } else if (an.compare(0, 6, "xmlns:") == 0) {
        scope[an.substr(6)] = av;
        el->declarations.emplace_back(an.substr(6), av);
      } else {
        raw.push_back({std::move(an), std::move(av)});
      }
    }
    scopes.push_back(std::move(scope));

    splitName(qname, el->prefix, el->local);
    if (const auto* uri = lookup(scopes, el->prefix)) {
      el->uri = *uri;
    } else if (!el->prefix.empty()) {
      corrupt("undeclared XML namespace prefix '" + el->prefix + "'");
    }
    for (auto& a : raw) {
      XmlAttribute attr;
      splitName(a.qname, attr.prefix, attr.local);
      // Unprefixed attributes have no namespace.
      if (!attr.prefix.empty()) {
        const auto* uri = lookup(scopes, attr.prefix);
        if (!uri) corrupt("undeclared XML namespace prefix '" + attr.prefix + "'");
        attr.uri = *uri;
      }
      attr.value = std::move(a.value);
      el->attributes.push_back(std::move(attr));
    }

    if (!empty) {
      for (;;) {
        if (pos_ >= s_.size()) corrupt("unterminated element <" + qname + ">");
        const char c = s_[pos_];
        if (c == '<') {
          if (lookingAt("</")) {
            pos_ += 2;
            const std::string end = name();
            if (end != qname) corrupt("mismatched end tag </" + end + "> for <" + qname + ">");
            skipWs();
            if (pos_ >= s_.size() || s_[pos_] != '>') corrupt("bad end tag");
            ++pos_;
            break;
          } else if (lookingAt("<!--")) {
            skipPast("-->");
          } else if (lookingAt("<![CDATA[")) {
            pos_ += 9;
            const auto p = s_.find("]]>", pos_);
            if (p == std::string_view::npos) corrupt("unterminated CDATA section");
            el->text.append(s_.substr(pos_, p - pos_));
            pos_ = p + 3;
          } else if (lookingAt("<?")) {
            skipPast("?>");
          } else if (lookingAt("<!")) {
            corrupt("unexpected declaration in XML content");
          } else {
            el->children.push_back(element(scopes, depth + 1));
          }
        } else if (c == '&') {
          entity(el->text);
        } else {
          const auto next = s_.find_first_of("<&", pos_);
          const auto end = next == std::string_view::npos ? s_.size() : next;
          el->text.append(s_.substr(pos_, end - pos_));
          pos_ = end;
        }
      }
    }
    scopes.pop_back();
    return el;
  }

  std::string_view s_;
  std::size_t pos_ = 0;
};

}  // namespace

std::unique_ptr<XmlElement> parseXml(std::string_view document) { return Parser(document).document(); }

std::string escapeXml(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (char c : text) {
    switch (c) {
      case '<':
        out += "&lt;";
        break;
      case '>':
        out += "&gt;";
        break;
      case '&':
        out += "&amp;";
        break;
      case '"':
        out += "&quot;";
        break;
      case '\r':
        out += "&#xD;";
        break;
      default:
        // Other control characters cannot be represented in XML 1.0.
        if (static_cast<unsigned char>(c) >= 0x20 || c == '\t' || c == '\n') out += c;
    }
  }
  return out;
}

}  // namespace photos::detail
