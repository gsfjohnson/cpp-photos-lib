#include "testing.hpp"

using namespace lumenlib;

namespace {

std::string packet(const std::string& body, const std::string& ns = "") {
  return "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
         "<rdf:Description rdf:about=\"\" xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
         "xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\"" +
         ns + ">" + body + "</rdf:Description></rdf:RDF></x:xmpmeta>";
}

std::string text(const std::string& path) {
  const Bytes b = testing::readData(path);
  return std::string(b.begin(), b.end());
}

// UTF-16 of ASCII text, in either byte order, with a byte-order mark.
std::string utf16(const std::string& s, bool big) {
  std::string out = big ? std::string("\xfe\xff") : std::string("\xff\xfe");
  for (char c : s) {
    if (big) out += '\0';
    out += c;
    if (!big) out += '\0';
  }
  return out;
}

}  // namespace

TEST(xmp_parses_the_fixture) {
  const XmpMetadata xmp = XmpMetadata::parse(text("photo.xmp"));
  CHECK_EQ(xmp.find("xmp:Rating")->summary(), "4");
  const auto& title = xmp.find("dc:title")->value();
  CHECK(title.kind() == XmpValue::Kind::langAlt);
  CHECK_EQ(*title.langText(), "Harbour at dusk");
  CHECK_EQ(*title.langText("de"), "Hafen");
  CHECK_EQ(*xmp.text("dc:title"), "Harbour at dusk");
  const auto& subject = xmp.find("dc:subject")->value();
  CHECK(subject.kind() == XmpValue::Kind::bag);
  CHECK((subject.items() == std::vector<std::string>{"harbour", "boats", testing::kKobenhavn}));
  CHECK_EQ(*xmp.text("dc:subject"), "harbour");
  CHECK(xmp.find("mwg-rs:Regions")->kind() == XmpValue::Kind::structure);
  CHECK(xmp.find("mwg-rs:Regions/mwg-rs:RegionList")->kind() == XmpValue::Kind::bag);
  CHECK_EQ(xmp.find("mwg-rs:Regions/mwg-rs:RegionList[1]/mwg-rs:Name")->summary(), "Ada");
  CHECK_EQ(xmp.find("mwg-rs:Regions/mwg-rs:RegionList[1]/mwg-rs:Area/stArea:x")->summary(), "0.5");
  CHECK_EQ(xmp.find("mwg-rs:Regions/mwg-rs:AppliedToDimensions/stDim:w")->summary(), "64");
  CHECK(!xmp.text("mwg-rs:Regions"));
  const auto* region = xmp.find("mwg-rs:Regions/mwg-rs:RegionList[1]/mwg-rs:Name");
  CHECK_EQ(region->prefix(), "mwg-rs");
  CHECK_EQ(region->name(), "Regions");
  CHECK(region->isUnder("mwg-rs:Regions"));
  CHECK(!region->isUnder("mwg-rs:Region"));
}

TEST(xmp_serialize_parse_round_trip) {
  const XmpMetadata xmp = XmpMetadata::parse(text("photo.xmp"));
  for (bool compact : {false, true}) {
    XmpWriteOptions options;
    options.compact = compact;
    const XmpMetadata back = XmpMetadata::parse(xmp.serialize(options));
    CHECK_EQ(back.size(), xmp.size());
    for (const auto& e : xmp) {
      const auto* b = back.find(e.path());
      CHECK(b != nullptr);
      CHECK(b->value() == e.value());
    }
  }
}

TEST(xmp_write_options) {
  XmpMetadata xmp;
  xmp.setText("xmp:Rating", "5");
  xmp.setItems("dc:subject", {"a"});
  const std::string wrapped = xmp.serialize();
  CHECK(wrapped.compare(0, 9, "<?xpacket") == 0);
  CHECK(wrapped.find("<?xpacket end=\"w\"?>") != std::string::npos);
  XmpWriteOptions options;
  options.packetWrapper = false;
  options.compact = true;
  const std::string compact = xmp.serialize(options);
  CHECK(compact.find("xpacket") == std::string::npos);
  CHECK(compact.find("xmp:Rating=\"5\"") != std::string::npos);  // an attribute
  CHECK(compact.find("<rdf:Bag>") != std::string::npos);         // arrays stay elements
  CHECK_EQ(*XmpMetadata::parse(compact).text("xmp:Rating"), "5");
}

TEST(xmp_building_from_paths) {
  XmpMetadata xmp;
  xmp.setItems("dc:subject", {"a", "b"});
  xmp.setText("dc:title", "Title & <more>");
  xmp.setText("xmp:Rating", "5");
  xmp.setText("Iptc4xmpExt:LocationShown[1]/Iptc4xmpExt:City", "Oslo");
  xmp.setText("Iptc4xmpExt:LocationShown[2]/Iptc4xmpExt:City", "Bergen");
  CHECK(xmp.find("dc:subject")->kind() == XmpValue::Kind::bag);
  CHECK(xmp.find("dc:title")->kind() == XmpValue::Kind::langAlt);

  const std::string s = xmp.serialize();
  CHECK(s.find("Title &amp; &lt;more&gt;") != std::string::npos);
  CHECK(s.find("xmlns:Iptc4xmpExt=\"http://iptc.org/std/Iptc4xmpExt/2008-02-29/\"") != std::string::npos);
  const XmpMetadata back = XmpMetadata::parse(s);
  CHECK_EQ(*back.find("dc:title")->value().langText(), "Title & <more>");
  CHECK_EQ(back.find("dc:subject")->summary(), "a, b");
  CHECK_EQ(back.find("Iptc4xmpExt:LocationShown[2]/Iptc4xmpExt:City")->summary(), "Bergen");
  CHECK(back.find("Iptc4xmpExt:LocationShown")->kind() == XmpValue::Kind::bag);
}

TEST(xmp_language_alternatives) {
  XmpMetadata xmp = XmpMetadata::parse(text("photo.xmp"));
  xmp.setLangText("dc:title", "x-default", "New");
  CHECK_EQ(*xmp.find("dc:title")->value().langText(), "New");
  CHECK_EQ(*xmp.find("dc:title")->value().langText("de"), "Hafen");  // kept
  xmp.setLangText("dc:title", "de", "");
  CHECK_EQ(xmp.find("dc:title")->value().languages().size(), 1u);
  xmp.setLangText("dc:title", "x-default", "");
  CHECK(!xmp.contains("dc:title"));
  // A property of another kind becomes a language alternative.
  xmp.setText("dc:description", "plain");
  xmp.find("dc:description")->setValue(XmpValue::text("plain"));
  xmp.setLangText("dc:description", "x-default", "alt");
  CHECK(xmp.find("dc:description")->kind() == XmpValue::Kind::langAlt);
}

TEST(xmp_remove_takes_nested_entries) {
  XmpMetadata xmp = XmpMetadata::parse(text("photo.xmp"));
  const auto before = xmp.size();
  CHECK_EQ(xmp.remove("mwg-rs:Regions"), 15u);
  CHECK_EQ(xmp.size(), before - 15);
  CHECK(xmp.find("mwg-rs:Regions/mwg-rs:RegionList[1]/mwg-rs:Name") == nullptr);
  // set() replaces what was nested under the property too.
  XmpMetadata again = XmpMetadata::parse(text("photo.xmp"));
  again.set("mwg-rs:Regions", XmpValue::text("gone"));
  CHECK_EQ(again.size(), before - 14);
}

TEST(xmp_attributes_resources_and_entities) {
  const XmpMetadata xmp = XmpMetadata::parse(
      packet("<dc:source rdf:resource=\"http://example.com/\"/><dc:format>image/jpeg&#x20;&amp;&#65;</dc:format>",
             " xmp:Label=\"Red\""));
  CHECK_EQ(xmp.find("xmp:Label")->summary(), "Red");
  CHECK_EQ(xmp.find("dc:source")->summary(), "http://example.com/");
  CHECK_EQ(xmp.find("dc:format")->summary(), "image/jpeg &A");
}

TEST(xmp_utf16_and_utf32_packets) {
  const std::string doc = packet("<dc:format>image/png</dc:format>");
  for (bool big : {false, true}) {
    CHECK_EQ(*XmpMetadata::parse(utf16(doc, big)).text("dc:format"), "image/png");
  }
  std::string utf32;
  for (char c : doc) utf32 += std::string(1, c) + std::string(3, '\0');
  CHECK_EQ(*XmpMetadata::parse(utf32).text("dc:format"), "image/png");
}

TEST(xmp_unknown_namespaces_are_registered) {
  const XmpMetadata xmp =
      XmpMetadata::parse(packet("<my:thing>v</my:thing>", " xmlns:my=\"http://example.com/my/1.0/\""));
  CHECK_EQ(xmp.find("my:thing")->summary(), "v");
  CHECK_EQ(*xmpNamespaceUri("my"), "http://example.com/my/1.0/");
  // Another URI with the same prefix gets a numbered prefix.
  const XmpMetadata other =
      XmpMetadata::parse(packet("<my:thing>w</my:thing>", " xmlns:my=\"http://example.com/other/\""));
  CHECK_EQ(other.find("my1:thing")->summary(), "w");
  // Known URIs keep the registry's prefix whatever the document calls them.
  const XmpMetadata renamed =
      XmpMetadata::parse(packet("<d:format>x</d:format>", " xmlns:d=\"http://purl.org/dc/elements/1.1/\""));
  CHECK(renamed.find("dc:format") != nullptr);
  const XmpMetadata iptc = XmpMetadata::parse(
      packet("<iptc:Location>x</iptc:Location>", " xmlns:iptc=\"http://iptc.org/std/Iptc4xmpCore/1.0/xmlns/\""));
  CHECK(iptc.find("Iptc4xmpCore:Location") != nullptr);
}

TEST(xmp_bad_paths_and_documents) {
  XmpMetadata xmp;
  CHECK_THROWS(xmp.entry("nosuchprefix:a"), ErrorCode::invalidArgument);
  CHECK_THROWS(xmp.entry("Xmp.dc.a"), ErrorCode::invalidArgument);
  CHECK_THROWS(xmp.entry("dc:a/b"), ErrorCode::invalidArgument);
  CHECK_THROWS(xmp.entry("dc:a[0]"), ErrorCode::invalidArgument);
  CHECK_THROWS(xmp.entry("dc:a/"), ErrorCode::invalidArgument);
  CHECK_THROWS(xmp.entry("subject"), ErrorCode::invalidArgument);
  CHECK_THROWS(XmpMetadata::parse("<x:xmpmeta"), ErrorCode::corruptData);
  CHECK_THROWS(XmpMetadata::parse(packet("<dc:format>&unknown;</dc:format>")), ErrorCode::corruptData);
  CHECK_THROWS(XmpMetadata::parse(packet("<dc:format>a</dc:title>")), ErrorCode::corruptData);
  CHECK_THROWS(XmpMetadata::parse(packet("<undeclared:x>a</undeclared:x>")), ErrorCode::corruptData);
}

TEST(xmp_doctype_entities_are_not_expanded) {
  // A "billion laughs" document: the DTD is skipped, so &lol2; is unknown.
  const std::string doc =
      "<?xml version=\"1.0\"?><!DOCTYPE lolz [<!ENTITY lol \"lol\"><!ENTITY lol2 \"&lol;&lol;&lol;\">]>" +
      packet("<dc:format>&lol2;</dc:format>");
  CHECK_THROWS(XmpMetadata::parse(doc), ErrorCode::corruptData);
}

TEST(xmp_deep_nesting_is_rejected) {
  std::string deep;
  for (int i = 0; i < 1000; ++i) deep += "<dc:a rdf:parseType=\"Resource\">";
  for (int i = 0; i < 1000; ++i) deep += "</dc:a>";
  CHECK_THROWS(XmpMetadata::parse(packet(deep)), ErrorCode::corruptData);
}

TEST(xmp_empty_packet) {
  const XmpMetadata xmp = XmpMetadata::parse("<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"/>");
  CHECK(xmp.empty());
  CHECK(XmpMetadata::parse(XmpMetadata().serialize()).empty());
}
