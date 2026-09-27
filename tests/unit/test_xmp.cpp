#include "testing.hpp"

using namespace photos;

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

}  // namespace

TEST(xmp_parses_the_fixture) {
  const XmpData xmp = XmpData::parse(text("photo.xmp"));
  CHECK_EQ(xmp.find("Xmp.xmp.Rating")->toString(), "4");
  const auto& title = xmp.find("Xmp.dc.title")->value();
  CHECK(title.kind() == XmpValue::Kind::langAlt);
  CHECK_EQ(*title.langText(), "Harbour at dusk");
  CHECK_EQ(*title.langText("de"), "Hafen");
  const auto& subject = xmp.find("Xmp.dc.subject")->value();
  CHECK(subject.kind() == XmpValue::Kind::bag);
  CHECK((subject.items() == std::vector<std::string>{"harbour", "boats", testing::kKobenhavn}));
  CHECK(xmp.find("Xmp.mwg-rs.Regions")->kind() == XmpValue::Kind::structure);
  CHECK(xmp.find("Xmp.mwg-rs.Regions/mwg-rs:RegionList")->kind() == XmpValue::Kind::bag);
  CHECK_EQ(xmp.find("Xmp.mwg-rs.Regions/mwg-rs:RegionList[1]/mwg-rs:Name")->toString(), "Ada");
  CHECK_EQ(xmp.find("Xmp.mwg-rs.Regions/mwg-rs:RegionList[1]/mwg-rs:Area/stArea:x")->toString(), "0.5");
  CHECK_EQ(xmp.find("Xmp.mwg-rs.Regions/mwg-rs:AppliedToDimensions/stDim:w")->toString(), "64");
}

TEST(xmp_serialize_parse_round_trip) {
  const XmpData xmp = XmpData::parse(text("photo.xmp"));
  const XmpData back = XmpData::parse(xmp.serialize());
  CHECK_EQ(back.size(), xmp.size());
  for (const auto& d : xmp) {
    const auto* b = back.find(d.key());
    CHECK(b != nullptr);
    CHECK(b->value() == d.value());
  }
}

TEST(xmp_building_from_keys) {
  XmpData xmp;
  xmp["Xmp.dc.subject"] = std::vector<std::string>{"a", "b"};
  xmp["Xmp.dc.title"] = "Title & <more>";
  xmp["Xmp.xmp.Rating"] = "5";
  xmp["Xmp.iptcExt.LocationShown[1]/iptcExt:City"] = "Oslo";
  xmp["Xmp.iptcExt.LocationShown[2]/iptcExt:City"] = "Bergen";
  CHECK(xmp.find("Xmp.dc.subject")->kind() == XmpValue::Kind::bag);
  CHECK(xmp.find("Xmp.dc.title")->kind() == XmpValue::Kind::langAlt);

  const std::string s = xmp.serialize();
  CHECK(s.find("Title &amp; &lt;more&gt;") != std::string::npos);
  const XmpData back = XmpData::parse(s);
  CHECK_EQ(*back.find("Xmp.dc.title")->value().langText(), "Title & <more>");
  CHECK_EQ(back.find("Xmp.dc.subject")->toString(), "a, b");
  CHECK_EQ(back.find("Xmp.iptcExt.LocationShown[2]/iptcExt:City")->toString(), "Bergen");
  CHECK(back.find("Xmp.iptcExt.LocationShown")->kind() == XmpValue::Kind::bag);
}

TEST(xmp_erase_removes_nested) {
  XmpData xmp = XmpData::parse(text("photo.xmp"));
  const auto before = xmp.size();
  CHECK_EQ(xmp.erase("Xmp.mwg-rs.Regions"), 15u);
  CHECK_EQ(xmp.size(), before - 15);
  CHECK(xmp.find("Xmp.mwg-rs.Regions/mwg-rs:RegionList[1]/mwg-rs:Name") == nullptr);
}

TEST(xmp_attributes_resources_and_entities) {
  const XmpData xmp = XmpData::parse(
      packet("<dc:source rdf:resource=\"http://example.com/\"/><dc:format>image/jpeg&#x20;&amp;&#65;</dc:format>",
             " xmp:Label=\"Red\""));
  CHECK_EQ(xmp.find("Xmp.xmp.Label")->toString(), "Red");
  CHECK_EQ(xmp.find("Xmp.dc.source")->toString(), "http://example.com/");
  CHECK_EQ(xmp.find("Xmp.dc.format")->toString(), "image/jpeg &A");
}

TEST(xmp_unknown_namespaces_are_registered) {
  const XmpData xmp = XmpData::parse(packet("<my:thing>v</my:thing>", " xmlns:my=\"http://example.com/my/1.0/\""));
  CHECK_EQ(xmp.find("Xmp.my.thing")->toString(), "v");
  CHECK_EQ(*xmpNamespaceUri("my"), "http://example.com/my/1.0/");
  // Another URI with the same prefix gets a numbered prefix.
  const XmpData other = XmpData::parse(packet("<my:thing>w</my:thing>", " xmlns:my=\"http://example.com/other/\""));
  CHECK_EQ(other.find("Xmp.my1.thing")->toString(), "w");
  // Known URIs keep the registry's prefix whatever the document calls them.
  const XmpData renamed =
      XmpData::parse(packet("<d:format>x</d:format>", " xmlns:d=\"http://purl.org/dc/elements/1.1/\""));
  CHECK(renamed.find("Xmp.dc.format") != nullptr);
}

TEST(xmp_bad_keys_and_documents) {
  XmpData xmp;
  CHECK_THROWS(xmp["Xmp.nosuchprefix.a"], ErrorCode::invalidArgument);
  CHECK_THROWS(xmp["Exif.dc.a"], ErrorCode::invalidArgument);
  CHECK_THROWS(xmp["Xmp.dc.a/b"], ErrorCode::invalidArgument);
  CHECK_THROWS(xmp["Xmp.dc.a[0]"], ErrorCode::invalidArgument);
  CHECK_THROWS(XmpData::parse("<x:xmpmeta"), ErrorCode::corruptData);
  CHECK_THROWS(XmpData::parse(packet("<dc:format>&unknown;</dc:format>")), ErrorCode::corruptData);
  CHECK_THROWS(XmpData::parse(packet("<dc:format>a</dc:title>")), ErrorCode::corruptData);
  CHECK_THROWS(XmpData::parse(packet("<undeclared:x>a</undeclared:x>")), ErrorCode::corruptData);
}

TEST(xmp_doctype_entities_are_not_expanded) {
  // A "billion laughs" document: the DTD is skipped, so &lol9; is unknown.
  const std::string doc =
      "<?xml version=\"1.0\"?><!DOCTYPE lolz [<!ENTITY lol \"lol\"><!ENTITY lol2 \"&lol;&lol;&lol;\">]>" +
      packet("<dc:format>&lol2;</dc:format>");
  CHECK_THROWS(XmpData::parse(doc), ErrorCode::corruptData);
}

TEST(xmp_deep_nesting_is_rejected) {
  std::string deep;
  for (int i = 0; i < 1000; ++i) deep += "<dc:a rdf:parseType=\"Resource\">";
  for (int i = 0; i < 1000; ++i) deep += "</dc:a>";
  CHECK_THROWS(XmpData::parse(packet(deep)), ErrorCode::corruptData);
}

TEST(xmp_empty_packet) {
  const XmpData xmp = XmpData::parse("<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"/>");
  CHECK(xmp.empty());
  CHECK(XmpData::parse(XmpData().serialize()).empty());
}
