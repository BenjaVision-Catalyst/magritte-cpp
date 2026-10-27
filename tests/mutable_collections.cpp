#include <fstream>
#include <iostream>
#include <magritte/magritte.hpp>
#include <nlohmann/json.hpp>
using namespace magritte;
using J = nlohmann::json;
int checks = 0;
void check(bool b, const char *message) {
  ++checks;
  if (!b)
    throw Error(message);
}
template <class F> void fails(F f) {
  bool caught = false;
  try {
    f();
  } catch (const Error &) {
    caught = true;
  }
  check(caught, "Expected collection failure");
}
std::shared_ptr<Description>
field(std::string id, std::string cls = "MAToManyScalarRelationDescription") {
  auto d = std::make_shared<Description>();
  d->id = d->key = id;
  d->className = cls;
  return d;
}
void replay(Graph &g) {
  auto &m = g.objects.at("model");
  auto v = m.values.at("a");
  check(v.sameCollection(m.values.at("b")), "Wire alias");
  auto values = v.elements();
  check(values.back().sameCollection(v), "Wire cycle");
  v.mutableCollection()->atPut(0, Value("cross-port"));
  check(m.values.at("b").elements()[0] == Value("cross-port"),
        "Wire in-place mutation");
  auto root = g.descriptions.at("root"), peer = g.descriptions.at("peer");
  check(root->children.storage() == peer->children.storage(),
        "Wire child backing alias");
  root->remove(*g.descriptions.at("b"));
  check(peer->children.size() == 1, "Wire shared child mutation");
}
int main(int argc, char **argv) {
  try {
    CollectionStore store;
    auto nested = store.create("ordered", "nested", {Value("inner")});
    auto value = store.create("ordered", "value", {nested});
    auto alias = value;
    auto copy = value.shallowCopy();
    check(value.sameCollection(alias), "Native alias");
    check(!value.sameCollection(copy) && valuesEqual(value, copy),
          "Shallow root copy");
    check(copy.elements()[0].sameCollection(nested), "Shallow child alias");
    nested.mutableCollection()->append(Value("changed"));
    check(copy.elements()[0].collectionSize() == 2,
          "Nested mutation visible in shallow copy");
    value.mutableCollection()->append(value);
    check(value.elements().back().sameCollection(value), "Self cycle");
    check(valuesEqual(value, value), "Cyclic identity equality");
    Predicate notEmpty;
    notEmpty.selector = "notEmpty";
    check(notEmpty.evaluate(value), "Finite predicate over cyclic collection");
    auto stringDescription = field("string", "MAStringDescription");
    auto stringValue = store.string("stringForValidation", "live");
    DictionaryModel stringModel;
    stringModel.values["string"] = stringValue;
    check(validate(*stringDescription, stringModel).empty(),
          "Mutable String kind validation");
    check(writeString(*stringDescription, stringValue) == "live",
          "Mutable String writer");
    stringDescription->properties["metadata:mutable"] = value;
    auto propertyCopy = copyDescription(stringDescription, "propertyCopy");
    check(propertyCopy.root->properties.at("metadata:mutable")
              .sameCollection(value),
          "Description property shallow alias");
    check(SortBlock("=").value(value, value) == Value(true),
          "Sort equality preserves cyclic identity");
    check(SortBlock("<").value(stringValue, Value("z")) == Value(true),
          "Mutable String ordering");
    stringDescription->display("yourself");
    check(stringDescription->displayStringFor(stringValue) == "live",
          "Mutable String display");
    stringDescription->display(std::make_shared<DisplayBlock>(
        [](const Value &v) { return std::to_string(v.collectionSize()); }));
    check(stringDescription->displayStringFor(value) == "2",
          "Display callback receives cyclic collection");
    auto schema = field("comparison", "MAContainer");
    schema->setChildren({stringDescription});
    DictionaryModel first, second;
    first.values["string"] = value;
    second.values["string"] = value;
    check(isSameAs(first, second, *schema),
          "Model equality on shared cyclic collection");
    stringDescription->properties["default"] = value;
    DictionaryModel initialized;
    initializeMagritte(initialized, *schema);
    check(initialized.values.at("string").sameCollection(value),
          "Initialization preserves declared collection alias");
    auto option = field("option", "MASingleOptionDescription");
    option->options = {value};
    option->properties["sorted"] = Value(false);
    option->properties["required"] = Value(true);
    auto optionValues = option->allOptionsWith(value).values();
    check(optionValues.size() == 1 && optionValues[0].sameCollection(value),
          "Static option membership preserves cyclic identity");
    auto cycleCopy = value.shallowCopy();
    check(valuesEqual(value, cycleCopy), "Shallow cyclic equality");
    // Two independently self-recursive structures have no terminating source
    // equality.
    auto x = store.create("ordered", "x"), y = store.create("ordered", "y");
    x.mutableCollection()->append(x);
    y.mutableCollection()->append(y);
    fails([&] { (void)valuesEqual(x, y); });
    auto fixed = store.create("array", "fixed", {Value(1LL)});
    fixed.mutableCollection()->atPut(0, Value(2LL));
    check(fixed.elements()[0] == Value(2LL), "Native Array indexed write");
    fails([&] { fixed.mutableCollection()->append(Value()); });
    auto text = store.string("text", "a😀");
    auto textCopy = text.shallowCopy();
    text.mutableCollection()->atPut(1, Value(Value::Character{"z"}));
    check(text.mutableCollection()->snapshot() == Value("az") &&
              textCopy.mutableCollection()->snapshot() == Value("a😀"),
          "Native String shallow copy");
    for (auto kind : {"set", "bag"}) {
      auto hashed =
          store.create(kind, std::string(kind), {Value(1LL), Value(1LL)});
      check(hashed.collectionSize() == (std::string(kind) == "set" ? 1 : 2),
            "Native hashed storage");
      hashed.mutableCollection()->remove(Value(1LL));
      check(hashed.collectionSize() == (std::string(kind) == "set" ? 0 : 1),
            "Hashed removal");
      fails([&] { hashed.mutableCollection()->append(value); });
    }
    CollectionStore foreign;
    auto foreignValue = foreign.create("ordered", "foreign");
    fails([&] { value.mutableCollection()->append(foreignValue); });
    fails([&] { (void)encodeValue(value); });
    fails([&] {
      (void)decodeValue(R"({"type":"collectionReference","value":"value"})");
    });
    Limits bytes;
    bytes.bytes = 100;
    fails([&] { store.string("boundedString", std::string(101, 'a'), bytes); });
    Limits tiny;
    tiny.items = 0;
    fails([&] { nested.mutableCollection()->append(Value(), tiny); });
    check(nested.collectionSize() == 2, "Failed mutation is atomic");
    std::weak_ptr<CollectionReference> weak;
    std::weak_ptr<detail::CollectionStorage> weakStorage;
    {
      CollectionStore local;
      auto cyclic = local.create("ordered", "released");
      cyclic.mutableCollection()->append(cyclic);
      weak = cyclic.mutableCollection();
      weakStorage = cyclic.mutableCollection()->storage();
    }
    check(weak.expired() && weakStorage.expired(),
          "Cyclic arena releases native root and storage");
    std::ifstream baseline(std::string(FIXTURES) +
                           "/editing-boundaries-reference.json");
    J historical;
    baseline >> historical;
    for (auto &row : historical["mutableCollectionAlias"]) {
      CollectionStore arena;
      auto v = arena.create("ordered", "live", {Value("base")});
      auto d = field("value");
      auto root = field("root", "MAContainer");
      root->setChildren({d});
      DictionaryModel model;
      model.values["value"] = v;
      std::unique_ptr<CachedMemento> m;
      if (row["memento"] == "MACheckedMemento")
        m = std::make_unique<CheckedMemento>(model, root);
      else
        m = std::make_unique<CachedMemento>(model, root);
      check(m->readUsing(*d).sameCollection(v),
            "Cache preserves collection alias");
      v.mutableCollection()->append(Value("outside"));
      check(m->hasChanges() == row["hasChanges"], "Historical hasChanges");
      check(J::parse(encodeValue(v.mutableCollection()->snapshot())) ==
                row["raw"],
            "Historical raw");
      check(J::parse(encodeValue(
                m->readUsing(*d).mutableCollection()->snapshot())) ==
                row["cache"],
            "Historical cache");
      if (auto checked = dynamic_cast<CheckedMemento *>(m.get())) {
        check(J::parse(encodeValue(checked->original()
                                       .at("value")
                                       .mutableCollection()
                                       ->snapshot())) == row["original"],
              "Historical checked original");
        check(checked->hasModelChangedElsewhere() ==
                      row["hasModelChangedElsewhere"] &&
                  checked->hasConflict() == row["hasConflict"],
              "Historical conflicts");
      }
    }
    std::ifstream scenariosFile(std::string(FIXTURES) +
                                "/mutable-collection-scenarios.json");
    J scenarios;
    scenariosFile >> scenarios;
    for (const auto &scenario : scenarios["cases"]) {
      auto cls = scenario["class"].get<std::string>();
      CollectionStore arena;
      auto inner = arena.create("ordered", "inner", {Value("base")});
      auto v = arena.create("ordered", "live", {inner});
      auto d = field("value", cls), root = field("root", "MAContainer");
      root->setChildren({d});
      DictionaryModel model;
      model.values["value"] = v;
      CheckedMemento m(model, root);
      bool copied = scenario["copyOriginal"];
      check(m.original().at("value").sameCollection(v) != copied,
            "Only multiple references copy originals");
      inner.mutableCollection()->append(Value("nested"));
      check(!m.hasModelChangedElsewhere(), "Original nested values are shared");
      v.mutableCollection()->append(Value("outer"));
      check(m.hasModelChangedElsewhere() == copied, "Original copy boundary");
      check(!m.hasChanges(), "Model/cache alias");
      auto replacement =
          arena.create("ordered", "replacement", {Value("replacement")});
      m.writeUsing(*d, replacement);
      check(m.hasChanges(), "Staged replacement");
      check(m.hasConflict() == copied, "Alias conflict rule");
      m.commit();
      check(model.values.at("value").sameCollection(replacement),
            "Commit preserves staged identity");
      m.reset();
      check(m.readUsing(*d).sameCollection(replacement), "Reset rebinds cache");
      check(!m.hasChanges() && !m.hasConflict() &&
                !m.hasModelChangedElsewhere(),
            "Reset state");
    }
    {
      CollectionStore arena;
      auto live = arena.create("ordered", "live", {Value("base")});
      auto d = field("value"), schema = field("schema", "MAContainer");
      schema->setChildren({d});
      DictionaryModel model;
      model.values["value"] = live;
      Limits cap;
      cap.nodes = 3;
      CheckedMemento m(model, schema, nullptr, cap);
      auto previous = m.original().at("value");
      arena.create("ordered", "filler");
      live.mutableCollection()->append(Value("outside"));
      fails([&] { m.reset(); });
      check(m.original().at("value").sameCollection(previous),
            "Failed original copy preserves previous originals");
      check(m.readUsing(*d).sameCollection(live),
            "Cache reset precedes original copy failure");
    }
    auto a = field("a"), b = field("b"), root = field("root", "MAContainer"),
         peer = field("peer", "MAContainer");
    DescriptionChildren backing({a, b});
    root->setChildren(backing);
    peer->setChildren(backing);
    root->remove(*b);
    check(peer->children.size() == 1 && backing.size() == 1,
          "Plain children assignment aliases native storage");
    root->setChildren({a, b});
    check(backing.size() == 1 && peer->children.size() == 1,
          "Replacement detaches backing");
    peer->setChildren(root->children);
    auto copied = copyDescription(root, "copied");
    check(copied.root->children.storage() != root->children.storage() &&
              copied.root->children[0] == a,
          "Container postCopy shallow boundary");
    auto priority = field("priority", "MAPriorityContainer");
    priority->setChildren(root->children);
    check(priority->children.storage() != root->children.storage(),
          "Priority assignment converts/copies backing");
    peer->setChildren(priority->children);
    check(peer->children.storage() == priority->children.storage(),
          "Plain can alias sorted backing");
    a->properties["priority"] = Value(10LL);
    auto c = field("c");
    c->properties["priority"] = Value(20LL);
    peer->add(c);
    check(priority->children.size() == 3, "Aliased sorted insertion");
    root->setChildren(DescriptionChildren({a, b}, "array"));
    fails([&] { root->add(c); });
    root->moveDown(*a);
    check(root->children[0] == b, "Fixed child storage swaps");
    peer->setChildren({b, a});
    check(!descriptionEqual(*root, *peer),
          "Backing species participates in container equality");
    root->removeAll();
    check(root->children.empty(), "RemoveAll replaces fixed backing");
    std::ifstream graphFixture(std::string(FIXTURES) +
                               "/mutable-collection-graph.json");
    std::string fixtureText((std::istreambuf_iterator<char>(graphFixture)), {});
    auto fixtureGraph = Graph::decode(fixtureText);
    replay(fixtureGraph);
    a->properties.clear();
    b->properties.clear();
    Graph graph;
    graph.format = "magritte.graph/4";
    graph.root = "root";
    root->setChildren({a, b});
    peer->setChildren(root->children);
    graph.descriptions = {{"root", root}, {"peer", peer}, {"a", a}, {"b", b}};
    auto live = graph.collections.create("ordered", "live", {Value("wire")});
    live.mutableCollection()->append(live);
    AdaptiveModel model;
    model.id = "model";
    model.description = root;
    model.values = {{"a", live}, {"b", live}};
    graph.objects["model"] = model;
    auto encoded = graph.encode();
    if (argc > 1)
      std::ofstream(argv[1]) << encoded;
    auto decoded = Graph::decode(encoded);
    replay(decoded);
    if (argc > 2) {
      std::ifstream in(argv[2]);
      std::string s((std::istreambuf_iterator<char>(in)), {});
      auto peerGraph = Graph::decode(s);
      replay(peerGraph);
    }
    auto malformed = J::parse(encoded);
    malformed["collections"][0]["elements"][1]["value"] = "missing";
    fails([&] { Graph::decode(malformed.dump()); });
    malformed = J::parse(encoded);
    malformed["descriptions"][0]["childrenCollection"] = "missing";
    fails([&] { Graph::decode(malformed.dump()); });
    CollectionStore externalOne, externalTwo;
    auto ext1 = externalOne.create("ordered", "same", {Value("one")}),
         ext2 = externalTwo.create("ordered", "same", {Value("two")});
    graph.objects.at("model").values = {{"a", ext1}, {"b", ext2}};
    auto adapted = Graph::decode(graph.encode());
    check(!adapted.objects.at("model").values.at("a").sameCollection(
              adapted.objects.at("model").values.at("b")),
          "Distinct native stores with equal labels stay distinct");
    auto bad = J::parse(encoded);
    bad["collections"][0]["kind"] = "unknown";
    fails([&] { Graph::decode(bad.dump()); });
    bad = J::parse(encoded);
    bad["collections"].push_back(bad["collections"][0]);
    fails([&] { Graph::decode(bad.dump()); });
    bad = J::parse(encoded);
    bad["childCollections"][0]["children"] = {"b", "a"};
    fails([&] { Graph::decode(bad.dump()); });
    Graph nullGraph;
    nullGraph.descriptions["null"] = nullptr;
    fails([&] { nullGraph.encode(); });
    graph.format = "magritte.graph/3";
    fails([&] { graph.encode(); });
    auto legacy = J::parse(encoded);
    legacy["format"] = "magritte.graph/3";
    fails([&] { Graph::decode(legacy.dump()); });
    std::cout << checks << " mutable collection checks passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "check " << checks << ": " << e.what() << '\n';
    return 1;
  }
}
