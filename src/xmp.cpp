#include <photos/error.hpp>
#include <photos/version.hpp>
#include <photos/xmp.hpp>

#include "bytes.hpp"
#include "strings.hpp"
#include "xml.hpp"

#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <set>

namespace photos {
namespace {

constexpr const char* kRdf = "http://www.w3.org/1999/02/22-rdf-syntax-ns#";

// ---- Namespace registry -------------------------------------------------------

struct Registry {
  std::mutex mutex;
  std::map<std::string, std::string, std::less<>> byPrefix;  // prefix -> uri
  std::map<std::string, std::string, std::less<>> byUri;     // uri -> prefix

  Registry() {
    // exiv2's prefixes, so keys match exiv2's (Iptc4xmpCore is "iptc").
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
        {"iptc", "http://iptc.org/std/Iptc4xmpCore/1.0/xmlns/"},
        {"iptcExt", "http://iptc.org/std/Iptc4xmpExt/2008-02-29/"},
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

// ---- Keys ---------------------------------------------------------------------

struct Step {
  std::string qname;      // prefix:local
  std::size_t index = 0;  // 1-based array item, 0 for none
};

// "Xmp.dc.subject" -> [{dc:subject}]; nested steps follow.
std::vector<Step> parseKey(std::string_view key) {
  const auto bad = [&](const char* why) {
    return Error(ErrorCode::invalidArgument, "invalid XMP key '" + std::string(key) + "': " + why);
  };
  if (key.substr(0, 4) != "Xmp.") throw bad("must start with Xmp.");
  const auto rest = key.substr(4);
  const auto dot = rest.find('.');
  if (dot == std::string_view::npos || dot == 0) throw bad("no prefix");
  const std::string prefix(rest.substr(0, dot));
  std::string_view path = rest.substr(dot + 1);
  if (path.empty()) throw bad("no property name");

  std::vector<Step> steps;
  bool first = true;
  while (!path.empty()) {
    const auto slash = path.find('/');
    std::string_view part = path.substr(0, slash);
    path = slash == std::string_view::npos ? std::string_view() : path.substr(slash + 1);
    if (slash != std::string_view::npos && path.empty()) throw bad("trailing '/'");
    Step step;
    const auto bracket = part.find('[');
    std::string_view name = part.substr(0, bracket);
    if (bracket != std::string_view::npos) {
      if (part.back() != ']') throw bad("bad array index");
      const auto n = detail::parseInt(part.substr(bracket + 1, part.size() - bracket - 2));
      if (!n || *n < 1 || *n > 100000) throw bad("bad array index");
      step.index = static_cast<std::size_t>(*n);
    }
    if (name.empty() || name.find('?') != std::string_view::npos) throw bad("bad step");
    if (first) {
      if (name.find(':') != std::string_view::npos) throw bad("bad property name");
      step.qname = prefix + ":" + std::string(name);
    } else {
      const auto colon = name.find(':');
      if (colon == std::string_view::npos || colon == 0 || colon + 1 == name.size()) throw bad("steps are prefix:name");
      step.qname = std::string(name);
    }
    steps.push_back(std::move(step));
    first = false;
  }
  return steps;
}

XmpValue::Kind knownKind(std::string_view key) {
  static const std::map<std::string, XmpValue::Kind, std::less<>> known = {
      {"Xmp.dc.contributor", XmpValue::Kind::bag},
      {"Xmp.dc.creator", XmpValue::Kind::seq},
      {"Xmp.dc.date", XmpValue::Kind::seq},
      {"Xmp.dc.description", XmpValue::Kind::langAlt},
      {"Xmp.dc.language", XmpValue::Kind::bag},
      {"Xmp.dc.publisher", XmpValue::Kind::bag},
      {"Xmp.dc.relation", XmpValue::Kind::bag},
      {"Xmp.dc.rights", XmpValue::Kind::langAlt},
      {"Xmp.dc.subject", XmpValue::Kind::bag},
      {"Xmp.dc.title", XmpValue::Kind::langAlt},
      {"Xmp.dc.type", XmpValue::Kind::bag},
      {"Xmp.xmp.Identifier", XmpValue::Kind::bag},
      {"Xmp.xmpRights.Owner", XmpValue::Kind::bag},
      {"Xmp.xmpRights.UsageTerms", XmpValue::Kind::langAlt},
      {"Xmp.photoshop.SupplementalCategories", XmpValue::Kind::bag},
      {"Xmp.lr.hierarchicalSubject", XmpValue::Kind::bag},
      {"Xmp.lr.weightedFlatSubject", XmpValue::Kind::bag},
      {"Xmp.iptc.Scene", XmpValue::Kind::bag},
      {"Xmp.iptc.SubjectCode", XmpValue::Kind::bag},
      {"Xmp.iptcExt.PersonInImage", XmpValue::Kind::bag},
      {"Xmp.digiKam.TagsList", XmpValue::Kind::seq},
      {"Xmp.MicrosoftPhoto.LastKeywordXMP", XmpValue::Kind::bag},
      {"Xmp.exif.ISOSpeedRatings", XmpValue::Kind::seq},
  };
  const auto it = known.find(key);
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
  explicit RdfReader(XmpData& out) : out_(out) {}

  void description(const XmlElement& desc) {
    for (const auto& a : desc.attributes) {
      if (isPropertyAttribute(a)) add(childKey("", a.uri, a.prefix, a.local), XmpValue::text(a.value));
    }
    for (const auto& c : desc.children) property(*c, childKey("", c->uri, c->prefix, c->local));
  }

 private:
  std::string childKey(const std::string& parent, const std::string& uri, const std::string& docPrefix,
                       const std::string& local) {
    if (uri.empty()) detail::corrupt("XMP property '" + local + "' has no namespace");
    const std::string prefix = prefixForUri(uri, docPrefix);
    if (parent.empty()) return "Xmp." + prefix + "." + local;
    return parent + "/" + prefix + ":" + local;
  }

  void add(const std::string& key, XmpValue value) { out_.add(XmpDatum(key, std::move(value))); }

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

  void structFields(const XmlElement& e, const std::string& key) {
    for (const auto& a : e.attributes) {
      if (isPropertyAttribute(a)) add(childKey(key, a.uri, a.prefix, a.local), XmpValue::text(a.value));
    }
    for (const auto& c : e.children) property(*c, childKey(key, c->uri, c->prefix, c->local));
  }

  void property(const XmlElement& e, const std::string& key) {
    if (const auto* pt = e.attribute(kRdf, "parseType")) {
      if (pt->value == "Resource") {
        add(key, XmpValue(XmpValue::Kind::structure));
        structFields(e, key);
        return;
      }
      if (pt->value == "Literal") {
        add(key, XmpValue::text(e.text));
        return;
      }
    }
    if (e.children.empty()) {
      bool hasFields = false;
      for (const auto& a : e.attributes) hasFields |= isPropertyAttribute(a);
      if (hasFields && detail::trim(e.text).empty()) {
        add(key, XmpValue(XmpValue::Kind::structure));
        structFields(e, key);
      } else {
        add(key, XmpValue::text(simpleText(e)));
      }
      return;
    }
    const XmlElement& c = *e.children.front();
    if (isRdf(c, "Bag") || isRdf(c, "Seq") || isRdf(c, "Alt")) {
      array(c, key);
    } else if (isRdf(c, "Description")) {
      add(key, XmpValue(XmpValue::Kind::structure));
      structFields(c, key);
    } else {
      add(key, XmpValue(XmpValue::Kind::structure));
      structFields(e, key);
    }
  }

  void array(const XmlElement& container, const std::string& key) {
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
      add(key, std::move(v));
      return;
    }
    if (allSimple) {
      std::vector<std::string> texts;
      for (const auto* li : items) texts.push_back(simpleText(*li));
      add(key, XmpValue::array(kind, std::move(texts)));
      return;
    }
    add(key, XmpValue(kind));
    std::size_t n = 0;
    for (const auto* li : items) {
      const std::string itemKey = key + "[" + std::to_string(++n) + "]";
      if (isSimple(*li)) {
        add(itemKey, XmpValue::text(simpleText(*li)));
      } else {
        property(*li, itemKey);
      }
    }
  }

  XmpData& out_;
};

const XmlElement* findRdf(const XmlElement& e, int depth = 0) {
  if (isRdf(e, "RDF")) return &e;
  if (depth > 8) return nullptr;
  for (const auto& c : e.children) {
    if (const auto* r = findRdf(*c, depth + 1)) return r;
  }
  return nullptr;
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

const char* toString(XmpValue::Kind kind) noexcept {
  switch (kind) {
    case XmpValue::Kind::text:
      return "XmpText";
    case XmpValue::Kind::bag:
      return "XmpBag";
    case XmpValue::Kind::seq:
      return "XmpSeq";
    case XmpValue::Kind::alt:
      return "XmpAlt";
    case XmpValue::Kind::langAlt:
      return "LangAlt";
    case XmpValue::Kind::structure:
      return "XmpStruct";
  }
  return "XmpText";
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

std::string XmpValue::toString() const {
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

// ---- XmpDatum -----------------------------------------------------------------

XmpDatum::XmpDatum(std::string key, XmpValue value) : key_(std::move(key)), value_(std::move(value)) {
  parseKey(key_);
  if (!xmpNamespaceUri(prefix())) {
    throw Error(ErrorCode::invalidArgument, "unknown XMP namespace prefix '" + prefix() + "' in " + key_);
  }
}

std::string XmpDatum::prefix() const {
  const auto dot = key_.find('.', 4);
  return key_.substr(4, dot - 4);
}

std::string XmpDatum::name() const {
  const auto dot = key_.find('.', 4);
  return key_.substr(dot + 1);
}

std::string XmpDatum::namespaceUri() const { return xmpNamespaceUri(prefix()).value_or(std::string()); }

XmpDatum& XmpDatum::operator=(std::string text) {
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
  return *this;
}

XmpDatum& XmpDatum::operator=(const std::vector<std::string>& items) {
  value_ = XmpValue::array(value_.isArray() ? value_.kind() : XmpValue::Kind::bag, items);
  return *this;
}

// ---- XmpData ------------------------------------------------------------------

XmpDatum& XmpData::operator[](std::string_view key) {
  if (auto* d = find(key)) return *d;
  data_.emplace_back(std::string(key), XmpValue(knownKind(key)));
  return data_.back();
}

void XmpData::add(XmpDatum datum) { data_.push_back(std::move(datum)); }

XmpDatum* XmpData::find(std::string_view key) {
  for (auto& d : data_) {
    if (d.key() == key) return &d;
  }
  return nullptr;
}

const XmpDatum* XmpData::find(std::string_view key) const {
  for (const auto& d : data_) {
    if (d.key() == key) return &d;
  }
  return nullptr;
}

std::size_t XmpData::erase(std::string_view key) {
  const auto before = data_.size();
  data_.erase(std::remove_if(data_.begin(), data_.end(),
                             [&](const XmpDatum& d) {
                               const auto& k = d.key();
                               if (k.size() < key.size() || k.compare(0, key.size(), key) != 0) return false;
                               return k.size() == key.size() || k[key.size()] == '/' || k[key.size()] == '[';
                             }),
              data_.end());
  return before - data_.size();
}

void XmpData::sortByKey() {
  std::stable_sort(data_.begin(), data_.end(), [](const XmpDatum& a, const XmpDatum& b) { return a.key() < b.key(); });
}

XmpData XmpData::parse(std::string_view packet) {
  XmpData out;
  const auto root = detail::parseXml(packet);
  const XmlElement* rdf = findRdf(*root);
  if (!rdf) {
    if (root->local == "xmpmeta") return out;
    detail::corrupt("XMP packet has no rdf:RDF element");
  }
  RdfReader reader(out);
  for (const auto& desc : rdf->children) reader.description(*desc);
  return out;
}

std::string XmpData::serialize(const XmpWriteOptions& options) const {
  Node root;
  for (const auto& d : data_) {
    const auto steps = parseKey(d.key());
    Node* cur = &root;
    for (std::size_t i = 0; i < steps.size(); ++i) {
      const bool last = i + 1 == steps.size();
      Node* child = cur->field(steps[i].qname);
      if (steps[i].index) {
        if (!child->kindSet) {
          child->value = XmpValue(XmpValue::Kind::bag);
          child->kindSet = true;
        } else if (!child->value.isArray()) {
          throw Error(ErrorCode::invalidArgument, d.key() + ": item of a property that is not an array");
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
  std::string body;
  for (const auto& f : root.fields) writer.node(body, *f, 3);
  // Nested nodes may add prefixes; collect them from the keys as well.
  for (const auto& d : data_) {
    for (const auto& s : parseKey(d.key())) writer.addPrefix(s.qname);
  }

  std::string out;
  if (!options.omitPacketWrapper) out += "<?xpacket begin=\"\xef\xbb\xbf\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n";
  out += "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\" x:xmptk=\"photos " PHOTOS_VERSION_STRING "\">\n";
  out += " <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n";
  out += "  <rdf:Description rdf:about=\"\"";
  for (const auto& prefix : writer.prefixes()) {
    if (prefix == "rdf" || prefix == "xml") continue;
    const auto uri = xmpNamespaceUri(prefix);
    if (!uri) throw Error(ErrorCode::invalidArgument, "unknown XMP namespace prefix '" + prefix + "'");
    out += "\n    xmlns:" + prefix + "=\"" + detail::escapeXml(*uri) + "\"";
  }
  if (body.empty()) {
    out += "/>\n";
  } else {
    out += ">\n" + body + "  </rdf:Description>\n";
  }
  out += " </rdf:RDF>\n</x:xmpmeta>\n";
  if (!options.omitPacketWrapper) {
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

}  // namespace photos
