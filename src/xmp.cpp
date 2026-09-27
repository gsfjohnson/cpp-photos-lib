#include <lumenlib/error.hpp>
#include <lumenlib/version.hpp>
#include <lumenlib/xmp.hpp>

#include "bytes.hpp"
#include "strings.hpp"
#include "xml.hpp"

#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <set>

namespace lumenlib {
namespace {

constexpr const char* kRdf = "http://www.w3.org/1999/02/22-rdf-syntax-ns#";

// ---- Namespace registry -------------------------------------------------------

struct Registry {
  std::mutex mutex;
  std::map<std::string, std::string, std::less<>> byPrefix;  // prefix -> uri
  std::map<std::string, std::string, std::less<>> byUri;     // uri -> prefix

  Registry() {
    // Each schema's own preferred prefix.
    const std::pair<const char*, const char*> builtins[] = {
        {"dc", "http://purl.org/dc/elements/1.1/"},
        {"xmp", "http://ns.adobe.com/xap/1.0/"},
        {"xmpRights", "http://ns.adobe.com/xap/1.0/rights/"},
        {"xmpMM", "http://ns.adobe.com/xap/1.0/mm/"},
        {"xmpBJ", "http://ns.adobe.com/xap/1.0/bj/"},
        {"xmpTPg", "http://ns.adobe.com/xap/1.0/t/pg/"},
        {"xmpDM", "http://ns.adobe.com/xmp/1.0/DynamicMedia/"},
        {"xmpidq", "http://ns.adobe.com/xmp/Identifier/qual/1.0/"},
        {"xmpNote", "http://ns.adobe.com/xmp/note/"},
        {"xmpG", "http://ns.adobe.com/xap/1.0/g/"},
        {"xmpGImg", "http://ns.adobe.com/xap/1.0/g/img/"},
        {"pdf", "http://ns.adobe.com/pdf/1.3/"},
        {"photoshop", "http://ns.adobe.com/photoshop/1.0/"},
        {"crs", "http://ns.adobe.com/camera-raw-settings/1.0/"},
        {"crss", "http://ns.adobe.com/camera-raw-saved-settings/1.0/"},
        {"tiff", "http://ns.adobe.com/tiff/1.0/"},
        {"exif", "http://ns.adobe.com/exif/1.0/"},
        {"exifEX", "http://cipa.jp/exif/1.0/"},
        {"aux", "http://ns.adobe.com/exif/1.0/aux/"},
        {"lr", "http://ns.adobe.com/lightroom/1.0/"},
        {"hdrgm", "http://ns.adobe.com/hdr-gain-map/1.0/"},
        {"Iptc4xmpCore", "http://iptc.org/std/Iptc4xmpCore/1.0/xmlns/"},
        {"Iptc4xmpExt", "http://iptc.org/std/Iptc4xmpExt/2008-02-29/"},
        {"plus", "http://ns.useplus.org/ldf/xmp/1.0/"},
        {"mwg-rs", "http://www.metadataworkinggroup.com/schemas/regions/"},
        {"mwg-kw", "http://www.metadataworkinggroup.com/schemas/keywords/"},
        {"mwg-coll", "http://www.metadataworkinggroup.com/schemas/collections/"},
        {"stArea", "http://ns.adobe.com/xmp/sType/Area#"},
        {"stDim", "http://ns.adobe.com/xap/1.0/sType/Dimensions#"},
        {"stEvt", "http://ns.adobe.com/xap/1.0/sType/ResourceEvent#"},
        {"stRef", "http://ns.adobe.com/xap/1.0/sType/ResourceRef#"},
        {"stVer", "http://ns.adobe.com/xap/1.0/sType/Version#"},
        {"stJob", "http://ns.adobe.com/xap/1.0/sType/Job#"},
        {"stFnt", "http://ns.adobe.com/xap/1.0/sType/Font#"},
        {"digiKam", "http://www.digikam.org/ns/1.0/"},
        {"MicrosoftPhoto", "http://ns.microsoft.com/photo/1.0/"},
        {"MP", "http://ns.microsoft.com/photo/1.2/"},
        {"MPRI", "http://ns.microsoft.com/photo/1.2/t/RegionInfo#"},
        {"MPReg", "http://ns.microsoft.com/photo/1.2/t/Region#"},
        {"acdsee", "http://ns.acdsee.com/iptc/1.0/"},
        {"GPano", "http://ns.google.com/photos/1.0/panorama/"},
        {"GCamera", "http://ns.google.com/photos/1.0/camera/"},
        {"GContainer", "http://ns.google.com/photos/1.0/container/"},
        {"GContainerItem", "http://ns.google.com/photos/1.0/container/item/"},
        {"apple-fi", "http://ns.apple.com/faceinfo/1.0/"},
    };
    for (const auto& [prefix, uri] : builtins) {
      byPrefix.emplace(prefix, uri);
      byUri.emplace(uri, prefix);
    }
  }
};

Registry& registry() {
  static Registry r;
  return r;
}

// The registered prefix for the URI, registering it under `preferred` (or a
// numbered variant) if it is new.
std::string prefixForUri(const std::string& uri, const std::string& preferred) {
  auto& r = registry();
  std::lock_guard<std::mutex> lock(r.mutex);
  if (auto it = r.byUri.find(uri); it != r.byUri.end()) return it->second;
  std::string base = preferred.empty() ? "ns" : preferred;
  std::string prefix = base;
  for (int n = 1; r.byPrefix.count(prefix); ++n) prefix = base + std::to_string(n);
  r.byPrefix.emplace(prefix, uri);
  r.byUri.emplace(uri, prefix);
  return prefix;
}

// ---- Paths --------------------------------------------------------------------

struct Step {
  std::string qname;      // prefix:local
  std::size_t index = 0;  // 1-based array item, 0 for none
};

// "a:b[2]/c:d" -> [{a:b, 2}, {c:d, 0}].
std::vector<Step> parsePath(std::string_view path) {
  const auto bad = [&](const char* why) {
    return Error(ErrorCode::invalidArgument, "invalid XMP path '" + std::string(path) + "': " + why);
  };
  if (path.empty()) throw bad("empty");
  std::vector<Step> steps;
  std::string_view rest = path;
  while (!rest.empty()) {
    const auto slash = rest.find('/');
    std::string_view part = rest.substr(0, slash);
    rest = slash == std::string_view::npos ? std::string_view() : rest.substr(slash + 1);
    if (slash != std::string_view::npos && rest.empty()) throw bad("trailing '/'");
    Step step;
    const auto bracket = part.find('[');
    std::string_view name = part.substr(0, bracket);
    if (bracket != std::string_view::npos) {
      if (part.back() != ']') throw bad("bad array index");
      const auto n = detail::parseInt(part.substr(bracket + 1, part.size() - bracket - 2));
      if (!n || *n < 1 || *n > 100000) throw bad("bad array index");
      step.index = static_cast<std::size_t>(*n);
    }
    const auto colon = name.find(':');
    if (colon == std::string_view::npos || colon == 0 || colon + 1 == name.size() ||
        name.find(':', colon + 1) != std::string_view::npos || name.find_first_of("?@.= ") != std::string_view::npos) {
      throw bad("steps are prefix:name");
    }
    step.qname = std::string(name);
    steps.push_back(std::move(step));
  }
  return steps;
}

std::string_view prefixOf(std::string_view path) { return path.substr(0, path.find(':')); }

// Whether `path` is `property` or lies under it.
bool under(std::string_view path, std::string_view property) {
  if (path.size() < property.size() || path.compare(0, property.size(), property) != 0) return false;
  return path.size() == property.size() || path[property.size()] == '/' || path[property.size()] == '[';
}

XmpValue::Kind knownKind(std::string_view path) {
  static const std::map<std::string, XmpValue::Kind, std::less<>> known = {
      {"dc:contributor", XmpValue::Kind::bag},
      {"dc:creator", XmpValue::Kind::seq},
      {"dc:date", XmpValue::Kind::seq},
      {"dc:description", XmpValue::Kind::langAlt},
      {"dc:language", XmpValue::Kind::bag},
      {"dc:publisher", XmpValue::Kind::bag},
      {"dc:relation", XmpValue::Kind::bag},
      {"dc:rights", XmpValue::Kind::langAlt},
      {"dc:subject", XmpValue::Kind::bag},
      {"dc:title", XmpValue::Kind::langAlt},
      {"dc:type", XmpValue::Kind::bag},
      {"xmp:Identifier", XmpValue::Kind::bag},
      {"xmpRights:Owner", XmpValue::Kind::bag},
      {"xmpRights:UsageTerms", XmpValue::Kind::langAlt},
      {"photoshop:SupplementalCategories", XmpValue::Kind::bag},
      {"lr:hierarchicalSubject", XmpValue::Kind::bag},
      {"lr:weightedFlatSubject", XmpValue::Kind::bag},
      {"Iptc4xmpCore:Scene", XmpValue::Kind::bag},
      {"Iptc4xmpCore:SubjectCode", XmpValue::Kind::bag},
      {"Iptc4xmpCore:AltTextAccessibility", XmpValue::Kind::langAlt},
      {"Iptc4xmpCore:ExtDescrAccessibility", XmpValue::Kind::langAlt},
      {"Iptc4xmpExt:PersonInImage", XmpValue::Kind::bag},
      {"Iptc4xmpExt:LocationCreated", XmpValue::Kind::bag},
      {"Iptc4xmpExt:LocationShown", XmpValue::Kind::bag},
      {"digiKam:TagsList", XmpValue::Kind::seq},
      {"MicrosoftPhoto:LastKeywordXMP", XmpValue::Kind::bag},
      {"exif:ISOSpeedRatings", XmpValue::Kind::seq},
  };
  const auto it = known.find(path);
  return it == known.end() ? XmpValue::Kind::text : it->second;
}

// ---- Parsing RDF --------------------------------------------------------------

using detail::XmlElement;

bool isRdf(const XmlElement& e, std::string_view local) { return e.uri == kRdf && e.local == local; }

bool isPropertyAttribute(const detail::XmlAttribute& a) {
  return a.uri != kRdf && a.uri != detail::kXmlNamespace && !a.uri.empty();
}

class RdfReader {
 public:
  explicit RdfReader(std::vector<XmpEntry>& out) : out_(out) {}

  void description(const XmlElement& desc) {
    for (const auto& a : desc.attributes) {
      if (isPropertyAttribute(a)) add(childPath("", a.uri, a.prefix, a.local), XmpValue::text(a.value));
    }
    for (const auto& c : desc.children) property(*c, childPath("", c->uri, c->prefix, c->local));
  }

 private:
  static std::string childPath(const std::string& parent, const std::string& uri, const std::string& docPrefix,
                               const std::string& local) {
    if (uri.empty()) detail::corrupt("XMP property '" + local + "' has no namespace");
    const std::string step = prefixForUri(uri, docPrefix) + ":" + local;
    return parent.empty() ? step : parent + "/" + step;
  }

  void add(const std::string& path, XmpValue value) { out_.emplace_back(path, std::move(value)); }

  static bool isSimple(const XmlElement& e) {
    if (!e.children.empty()) return false;
    for (const auto& a : e.attributes) {
      if (isPropertyAttribute(a) || (a.uri == kRdf && a.local == "parseType")) return false;
    }
    return true;
  }

  static std::string simpleText(const XmlElement& e) {
    if (const auto* res = e.attribute(kRdf, "resource")) return res->value;
    return e.text;
  }

  void structFields(const XmlElement& e, const std::string& path) {
    for (const auto& a : e.attributes) {
      if (isPropertyAttribute(a)) add(childPath(path, a.uri, a.prefix, a.local), XmpValue::text(a.value));
    }
    for (const auto& c : e.children) property(*c, childPath(path, c->uri, c->prefix, c->local));
  }

  void property(const XmlElement& e, const std::string& path) {
    if (const auto* pt = e.attribute(kRdf, "parseType")) {
      if (pt->value == "Resource") {
        add(path, XmpValue(XmpValue::Kind::structure));
        structFields(e, path);
        return;
      }
      if (pt->value == "Literal") {
        add(path, XmpValue::text(e.text));
        return;
      }
    }
    if (e.children.empty()) {
      bool hasFields = false;
      for (const auto& a : e.attributes) hasFields |= isPropertyAttribute(a);
      if (hasFields && detail::trim(e.text).empty()) {
        add(path, XmpValue(XmpValue::Kind::structure));
        structFields(e, path);
      } else {
        add(path, XmpValue::text(simpleText(e)));
      }
      return;
    }
    const XmlElement& c = *e.children.front();
    if (isRdf(c, "Bag") || isRdf(c, "Seq") || isRdf(c, "Alt")) {
      array(c, path);
    } else if (isRdf(c, "Description")) {
      add(path, XmpValue(XmpValue::Kind::structure));
      structFields(c, path);
    } else {
      add(path, XmpValue(XmpValue::Kind::structure));
      structFields(e, path);
    }
  }

  void array(const XmlElement& container, const std::string& path) {
    const auto kind = isRdf(container, "Bag")   ? XmpValue::Kind::bag
                      : isRdf(container, "Seq") ? XmpValue::Kind::seq
                                                : XmpValue::Kind::alt;
    std::vector<const XmlElement*> items;
    for (const auto& li : container.children) {
      if (isRdf(*li, "li")) items.push_back(li.get());
    }
    bool allSimple = true, allLang = !items.empty();
    for (const auto* li : items) {
      allSimple &= isSimple(*li);
      allLang &= li->attribute(detail::kXmlNamespace, "lang") != nullptr;
    }
    if (allSimple && allLang && kind == XmpValue::Kind::alt) {
      XmpValue v(XmpValue::Kind::langAlt);
      for (const auto* li : items) v.setLangText(li->attribute(detail::kXmlNamespace, "lang")->value, simpleText(*li));
      add(path, std::move(v));
      return;
    }
    if (allSimple) {
      std::vector<std::string> texts;
      for (const auto* li : items) texts.push_back(simpleText(*li));
      add(path, XmpValue::array(kind, std::move(texts)));
      return;
    }
    add(path, XmpValue(kind));
    std::size_t n = 0;
    for (const auto* li : items) {
      const std::string itemPath = path + "[" + std::to_string(++n) + "]";
      if (isSimple(*li)) {
        add(itemPath, XmpValue::text(simpleText(*li)));
      } else {
        property(*li, itemPath);
      }
    }
  }

  std::vector<XmpEntry>& out_;
};

const XmlElement* findRdf(const XmlElement& e, int depth = 0) {
  if (isRdf(e, "RDF")) return &e;
  if (depth > 8) return nullptr;
  for (const auto& c : e.children) {
    if (const auto* r = findRdf(*c, depth + 1)) return r;
  }
  return nullptr;
}

// UTF-16 or UTF-32 (by byte-order mark, or by the NULs around the first '<')
// to UTF-8; UTF-8 as it is.
std::string toUtf8(std::string_view packet) {
  const auto b = [&](std::size_t i) { return i < packet.size() ? static_cast<unsigned char>(packet[i]) : 0x100u; };
  int unit = 0;
  bool big = false;
  std::size_t start = 0;
  if (b(0) == 0 && b(1) == 0 && b(2) == 0xfe && b(3) == 0xff) {
    unit = 4, big = true, start = 4;
  } else if (b(0) == 0xff && b(1) == 0xfe && b(2) == 0 && b(3) == 0) {
    unit = 4, start = 4;
  } else if (b(0) == 0xfe && b(1) == 0xff) {
    unit = 2, big = true, start = 2;
  } else if (b(0) == 0xff && b(1) == 0xfe) {
    unit = 2, start = 2;
  } else if (b(0) == 0 && b(1) == 0 && b(2) == 0 && b(3) == '<') {
    unit = 4, big = true;
  } else if (b(0) == '<' && b(1) == 0 && b(2) == 0 && b(3) == 0) {
    unit = 4;
  } else if (b(0) == 0 && b(1) == '<') {
    unit = 2, big = true;
  } else if (b(0) == '<' && b(1) == 0) {
    unit = 2;
  }
  if (unit == 0) return std::string(packet);
  std::string out;
  const auto put = [&](std::uint32_t cp) {
    if (cp < 0x80) {
      out += static_cast<char>(cp);
    } else if (cp < 0x800) {
      out += static_cast<char>(0xc0 | (cp >> 6));
      out += static_cast<char>(0x80 | (cp & 0x3f));
    } else if (cp < 0x10000) {
      out += static_cast<char>(0xe0 | (cp >> 12));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
      out += static_cast<char>(0x80 | (cp & 0x3f));
    } else if (cp <= 0x10ffff) {
      out += static_cast<char>(0xf0 | (cp >> 18));
      out += static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
      out += static_cast<char>(0x80 | (cp & 0x3f));
    } else {
      detail::corrupt("invalid character in XMP packet");
    }
  };
  const auto get = [&](std::size_t i, int n) {
    std::uint32_t v = 0;
    for (int k = 0; k < n; ++k) {
      const std::uint32_t byte = b(i + static_cast<std::size_t>(big ? k : n - 1 - k));
      v = (v << 8) | byte;
    }
    return v;
  };
  const std::size_t u = static_cast<std::size_t>(unit);
  for (std::size_t i = start; i + u <= packet.size(); i += u) {
    std::uint32_t cp = get(i, unit);
    if (unit == 2 && cp >= 0xd800 && cp <= 0xdbff && i + 4 <= packet.size()) {
      const std::uint32_t lo = get(i + 2, 2);
      if (lo >= 0xdc00 && lo <= 0xdfff) {
        cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
        i += 2;
      }
    }
    put(cp);
  }
  return out;
}

// ---- Serialising --------------------------------------------------------------

struct Node {
  std::string qname;
  bool kindSet = false;
  XmpValue value;
  std::vector<std::unique_ptr<Node>> fields;
  std::vector<std::unique_ptr<Node>> items;

  Node* field(const std::string& name) {
    for (auto& f : fields) {
      if (f->qname == name) return f.get();
    }
    fields.push_back(std::make_unique<Node>());
    fields.back()->qname = name;
    return fields.back().get();
  }
  XmpValue::Kind kind() const { return value.kind(); }
  bool isSimpleText() const { return kind() == XmpValue::Kind::text && fields.empty(); }
};

class Writer {
 public:
  void addPrefix(const std::string& qname) {
    const auto colon = qname.find(':');
    if (colon != std::string::npos) prefixes_.insert(qname.substr(0, colon));
  }

  void node(std::string& out, const Node& n, int indent) {
    const std::string pad(static_cast<std::size_t>(indent), ' ');
    addPrefix(n.qname);
    const auto kind = n.kind();
    if (kind == XmpValue::Kind::structure || (!n.fields.empty() && !n.value.isArray())) {
      if (n.fields.empty()) {
        out += pad + "<" + n.qname + " rdf:parseType=\"Resource\"/>\n";
        return;
      }
      out += pad + "<" + n.qname + " rdf:parseType=\"Resource\">\n";
      for (const auto& f : n.fields) node(out, *f, indent + 1);
      out += pad + "</" + n.qname + ">\n";
      return;
    }
    if (kind == XmpValue::Kind::text) {
      out += pad + "<" + n.qname + ">" + detail::escapeXml(n.value.text()) + "</" + n.qname + ">\n";
      return;
    }
    const char* container = kind == XmpValue::Kind::bag   ? "rdf:Bag"
                            : kind == XmpValue::Kind::seq ? "rdf:Seq"
                                                          : "rdf:Alt";
    out += pad + "<" + n.qname + ">\n";
    out += pad + " <" + container + ">\n";
    if (kind == XmpValue::Kind::langAlt) {
      for (const auto& [lang, text] : n.value.languages()) {
        out +=
            pad + "  <rdf:li xml:lang=\"" + detail::escapeXml(lang) + "\">" + detail::escapeXml(text) + "</rdf:li>\n";
      }
    } else if (!n.items.empty()) {
      for (const auto& item : n.items) node(out, *item, indent + 2);
    } else {
      for (const auto& text : n.value.items()) {
        out += pad + "  <rdf:li>" + detail::escapeXml(text) + "</rdf:li>\n";
      }
    }
    out += pad + " </" + std::string(container) + ">\n";
    out += pad + "</" + n.qname + ">\n";
  }

  const std::set<std::string>& prefixes() const { return prefixes_; }

 private:
  std::set<std::string> prefixes_;
};

}  // namespace

// ---- XmpValue -----------------------------------------------------------------

const char* xmpKindName(XmpValue::Kind kind) noexcept {
  switch (kind) {
    case XmpValue::Kind::text:
      return "text";
    case XmpValue::Kind::bag:
      return "bag";
    case XmpValue::Kind::seq:
      return "seq";
    case XmpValue::Kind::alt:
      return "alt";
    case XmpValue::Kind::langAlt:
      return "lang-alt";
    case XmpValue::Kind::structure:
      return "struct";
  }
  return "text";
}

XmpValue XmpValue::text(std::string value) {
  XmpValue v(Kind::text);
  v.text_ = std::move(value);
  return v;
}

XmpValue XmpValue::array(Kind kind, std::vector<std::string> items) {
  if (kind != Kind::bag && kind != Kind::seq && kind != Kind::alt) {
    throw Error(ErrorCode::invalidArgument, "XmpValue::array needs bag, seq or alt");
  }
  XmpValue v(kind);
  v.items_ = std::move(items);
  return v;
}

XmpValue XmpValue::langAlt(std::string xDefault) {
  XmpValue v(Kind::langAlt);
  v.setLangText("x-default", std::move(xDefault));
  return v;
}

std::optional<std::string> XmpValue::langText(std::string_view lang) const {
  for (const auto& [l, t] : langs_) {
    if (l == lang) return t;
  }
  for (const auto& [l, t] : langs_) {
    if (l == "x-default") return t;
  }
  if (!langs_.empty()) return langs_.front().second;
  return std::nullopt;
}

void XmpValue::setLangText(std::string_view lang, std::string text) {
  for (auto& [l, t] : langs_) {
    if (l == lang) {
      t = std::move(text);
      return;
    }
  }
  if (lang == "x-default") {
    langs_.insert(langs_.begin(), {std::string(lang), std::move(text)});
  } else {
    langs_.emplace_back(std::string(lang), std::move(text));
  }
}

std::string XmpValue::summary() const {
  switch (kind_) {
    case Kind::text:
      return text_;
    case Kind::bag:
    case Kind::seq:
    case Kind::alt: {
      std::string out;
      for (std::size_t i = 0; i < items_.size(); ++i) {
        if (i) out += ", ";
        out += items_[i];
      }
      return out;
    }
    case Kind::langAlt: {
      std::string out;
      for (std::size_t i = 0; i < langs_.size(); ++i) {
        if (i) out += ", ";
        out += "lang=\"" + langs_[i].first + "\" " + langs_[i].second;
      }
      return out;
    }
    case Kind::structure:
      return {};
  }
  return {};
}

bool operator==(const XmpValue& a, const XmpValue& b) noexcept {
  return a.kind_ == b.kind_ && a.text_ == b.text_ && a.items_ == b.items_ && a.langs_ == b.langs_;
}

// ---- XmpEntry -----------------------------------------------------------------

XmpEntry::XmpEntry(std::string path, XmpValue value) : path_(std::move(path)), value_(std::move(value)) {
  for (const auto& step : parsePath(path_)) {
    const auto prefix = std::string(prefixOf(step.qname));
    if (!xmpNamespaceUri(prefix)) {
      throw Error(ErrorCode::invalidArgument, "unknown XMP namespace prefix '" + prefix + "' in " + path_);
    }
  }
}

std::string XmpEntry::prefix() const { return std::string(prefixOf(path_)); }

std::string XmpEntry::name() const {
  const auto colon = path_.find(':');
  const auto end = path_.find_first_of("/[", colon);
  return path_.substr(colon + 1, end == std::string::npos ? std::string::npos : end - colon - 1);
}

std::string XmpEntry::namespaceUri() const { return xmpNamespaceUri(prefix()).value_or(std::string()); }

bool XmpEntry::isUnder(std::string_view property) const noexcept { return under(path_, property); }

void XmpEntry::setText(std::string text) {
  switch (value_.kind()) {
    case XmpValue::Kind::bag:
    case XmpValue::Kind::seq:
    case XmpValue::Kind::alt:
      value_.items() = {std::move(text)};
      break;
    case XmpValue::Kind::langAlt:
      value_.setLangText("x-default", std::move(text));
      break;
    default:
      value_ = XmpValue::text(std::move(text));
      break;
  }
}

void XmpEntry::setItems(std::vector<std::string> items) {
  value_ = XmpValue::array(value_.isArray() ? value_.kind() : XmpValue::Kind::bag, std::move(items));
}

// ---- XmpMetadata --------------------------------------------------------------

XmpEntry* XmpMetadata::find(std::string_view path) {
  for (auto& e : entries_) {
    if (e.path() == path) return &e;
  }
  return nullptr;
}

const XmpEntry* XmpMetadata::find(std::string_view path) const { return const_cast<XmpMetadata*>(this)->find(path); }

std::optional<std::string> XmpMetadata::text(std::string_view path) const {
  const auto* e = find(path);
  if (!e) return std::nullopt;
  const auto& v = e->value();
  switch (v.kind()) {
    case XmpValue::Kind::text:
      return v.text();
    case XmpValue::Kind::langAlt:
      return v.langText();
    case XmpValue::Kind::bag:
    case XmpValue::Kind::seq:
    case XmpValue::Kind::alt:
      if (v.items().empty()) return std::nullopt;
      return v.items().front();
    case XmpValue::Kind::structure:
      return std::nullopt;
  }
  return std::nullopt;
}

XmpEntry& XmpMetadata::entry(std::string_view path) {
  if (auto* e = find(path)) return *e;
  entries_.emplace_back(std::string(path), XmpValue(knownKind(path)));
  return entries_.back();
}

XmpEntry& XmpMetadata::set(std::string_view path, XmpValue value) {
  XmpEntry replacement{std::string(path), std::move(value)};  // validates first
  // Keep the property's place among the others.
  std::size_t at = entries_.size();
  for (std::size_t i = 0; i < entries_.size(); ++i) {
    if (entries_[i].isUnder(path)) {
      at = i;
      break;
    }
  }
  remove(path);
  at = std::min(at, entries_.size());
  entries_.insert(entries_.begin() + static_cast<std::ptrdiff_t>(at), std::move(replacement));
  return entries_[at];
}

XmpEntry& XmpMetadata::setText(std::string_view path, std::string text) {
  XmpEntry& e = entry(path);
  e.setText(std::move(text));
  return e;
}

XmpEntry& XmpMetadata::setItems(std::string_view path, std::vector<std::string> items) {
  const auto* existing = find(path);
  const auto kind = existing && existing->value().isArray() ? existing->kind()
                    : XmpValue(knownKind(path)).isArray()   ? knownKind(path)
                                                            : XmpValue::Kind::bag;
  return set(path, XmpValue::array(kind, std::move(items)));
}

void XmpMetadata::setLangText(std::string_view path, std::string_view lang, std::string text) {
  XmpValue value(XmpValue::Kind::langAlt);
  if (const auto* e = find(path); e && e->kind() == XmpValue::Kind::langAlt) value = e->value();
  if (text.empty()) {
    XmpValue kept(XmpValue::Kind::langAlt);
    for (const auto& [l, t] : value.languages()) {
      if (l != lang) kept.setLangText(l, t);
    }
    if (kept.languages().empty()) {
      remove(path);
      return;
    }
    set(path, std::move(kept));
    return;
  }
  value.setLangText(lang, std::move(text));
  set(path, std::move(value));
}

void XmpMetadata::append(XmpEntry entry) { entries_.push_back(std::move(entry)); }

std::size_t XmpMetadata::remove(std::string_view path) {
  return removeIf([&](const XmpEntry& e) { return e.isUnder(path); });
}

void XmpMetadata::sort() {
  std::stable_sort(entries_.begin(), entries_.end(),
                   [](const XmpEntry& a, const XmpEntry& b) { return a.path() < b.path(); });
}

XmpMetadata XmpMetadata::parse(std::string_view packet) {
  const std::string utf8 = toUtf8(packet);
  XmpMetadata out;
  const auto root = detail::parseXml(utf8);
  const XmlElement* rdf = findRdf(*root);
  if (!rdf) {
    if (root->local == "xmpmeta") return out;
    detail::corrupt("XMP packet has no rdf:RDF element");
  }
  RdfReader reader(out.entries_);
  for (const auto& desc : rdf->children) reader.description(*desc);
  return out;
}

std::string XmpMetadata::serialize(const XmpWriteOptions& options) const {
  Node root;
  for (const auto& d : entries_) {
    const auto steps = parsePath(d.path());
    Node* cur = &root;
    for (std::size_t i = 0; i < steps.size(); ++i) {
      const bool last = i + 1 == steps.size();
      Node* child = cur->field(steps[i].qname);
      if (steps[i].index) {
        if (!child->kindSet) {
          child->value = XmpValue(XmpValue::Kind::bag);
          child->kindSet = true;
        } else if (!child->value.isArray()) {
          throw Error(ErrorCode::invalidArgument, d.path() + ": item of a property that is not an array");
        }
        while (child->items.size() < steps[i].index) {
          child->items.push_back(std::make_unique<Node>());
          child->items.back()->qname = "rdf:li";
        }
        cur = child->items[steps[i].index - 1].get();
      } else {
        cur = child;
      }
      if (last) {
        cur->value = d.value();
        cur->kindSet = true;
      } else if (!cur->kindSet) {
        cur->value = XmpValue(XmpValue::Kind::structure);
        cur->kindSet = true;
      }
    }
  }

  Writer writer;
  std::string attributes, body;
  for (const auto& f : root.fields) {
    if (options.compact && f->isSimpleText()) {
      writer.addPrefix(f->qname);
      attributes += "\n   " + f->qname + "=\"" + detail::escapeXml(f->value.text()) + "\"";
    } else {
      writer.node(body, *f, 3);
    }
  }
  // Nested nodes may add prefixes; collect them from the paths as well.
  for (const auto& d : entries_) {
    for (const auto& s : parsePath(d.path())) writer.addPrefix(s.qname);
  }

  std::string out;
  if (options.packetWrapper) out += "<?xpacket begin=\"\xef\xbb\xbf\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n";
  out += "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\" x:xmptk=\"lumenlib " LUMENLIB_VERSION_STRING "\">\n";
  out += " <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n";
  out += "  <rdf:Description rdf:about=\"\"";
  for (const auto& prefix : writer.prefixes()) {
    if (prefix == "rdf" || prefix == "xml") continue;
    const auto uri = xmpNamespaceUri(prefix);
    if (!uri) throw Error(ErrorCode::invalidArgument, "unknown XMP namespace prefix '" + prefix + "'");
    out += "\n    xmlns:" + prefix + "=\"" + detail::escapeXml(*uri) + "\"";
  }
  out += attributes;
  if (body.empty()) {
    out += "/>\n";
  } else {
    out += ">\n" + body + "  </rdf:Description>\n";
  }
  out += " </rdf:RDF>\n</x:xmpmeta>\n";
  if (options.packetWrapper) {
    const std::string line(99, ' ');
    for (std::size_t n = 0; n < options.padding; n += 100) out += line + "\n";
    out += "<?xpacket end=\"w\"?>";
  }
  return out;
}

void registerXmpNamespace(const std::string& uri, const std::string& prefix) {
  if (uri.empty() || prefix.empty() || prefix.find_first_of(":. /[]") != std::string::npos) {
    throw Error(ErrorCode::invalidArgument, "bad XMP namespace '" + prefix + "' = '" + uri + "'");
  }
  auto& r = registry();
  std::lock_guard<std::mutex> lock(r.mutex);
  if (auto it = r.byPrefix.find(prefix); it != r.byPrefix.end()) {
    r.byUri.erase(it->second);
    r.byPrefix.erase(it);
  }
  if (auto it = r.byUri.find(uri); it != r.byUri.end()) {
    r.byPrefix.erase(it->second);
    r.byUri.erase(it);
  }
  r.byPrefix.emplace(prefix, uri);
  r.byUri.emplace(uri, prefix);
}

std::optional<std::string> xmpNamespaceUri(std::string_view prefix) {
  auto& r = registry();
  std::lock_guard<std::mutex> lock(r.mutex);
  const auto it = r.byPrefix.find(prefix);
  if (it == r.byPrefix.end()) return std::nullopt;
  return it->second;
}

std::optional<std::string> xmpNamespacePrefix(std::string_view uri) {
  auto& r = registry();
  std::lock_guard<std::mutex> lock(r.mutex);
  const auto it = r.byUri.find(uri);
  if (it == r.byUri.end()) return std::nullopt;
  return it->second;
}

}  // namespace lumenlib
