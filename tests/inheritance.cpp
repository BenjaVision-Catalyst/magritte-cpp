#include <fstream>
#include <iostream>
#include <magritte/magritte.hpp>
#include <nlohmann/json.hpp>
using namespace magritte;
using J = nlohmann::json;
int checks = 0;
void check(bool ok, const std::string &message) {
  ++checks;
  if (!ok)
    throw Error(message);
}
template <class F> void fails(F f) {
  bool caught = false;
  try {
    f();
  } catch (const Error &) {
    caught = true;
  }
  check(caught, "Expected refusal");
}
Value value(const J &j) { return decodeValue(j.dump()); }
struct Probe : Visitor {
  std::string dispatch;
#define VISIT(Name)                                                            \
  void visit##Name(Description &) override { dispatch = "visit" #Name; }
  VISIT(Container)
  VISIT(PriorityContainer) VISIT(StringDescription) VISIT(SymbolDescription)
      VISIT(MemoDescription) VISIT(PasswordDescription) VISIT(TokenDescription)
          VISIT(NumberDescription) VISIT(BooleanDescription)
              VISIT(ToOneRelationDescription) VISIT(ToManyRelationDescription)
                  VISIT(ToManyScalarRelationDescription)
                      VISIT(SingleOptionDescription)
                          VISIT(MultipleOptionDescription)
                              VISIT(ClassDescription)
#undef VISIT
};
int main(int argc, char **argv) {
  try {
    J fixture;
    std::ifstream(std::string(FIXTURES) + "/inheritance-scenarios.json") >>
        fixture;
    auto &registry = ClassRegistry::global();
    std::vector<ClassInfo> definitions;
    for (const auto &r : fixture["cases"]) {
      definitions.emplace_back(r["middle"], r["base"], true,
                               r["label"].get<std::string>());
      definitions.emplace_back(r["name"], r["middle"], false,
                               r["label"].get<std::string>());
    }
    registry.registerClasses(definitions);
    Graph exported;
    exported.format = "magritte.graph/4";
    exported.root = "root";
    auto root = std::make_shared<Description>();
    root->id = "root";
    root->className = "MAContainer";
    exported.descriptions[root->id] = root;
    for (const auto &r : fixture["cases"]) {
      auto d = std::make_shared<Description>();
      d->id = d->key = r["name"];
      d->className = r["name"];
      for (auto p = r["properties"].begin(); p != r["properties"].end(); ++p)
        d->properties[p.key()] = value(p.value());
      check(d->isContainer() == r["container"] &&
                d->isRelation() == r["relation"] &&
                d->isOption() == r["option"] &&
                d->isMultiple() == r["multiple"] &&
                d->isReference() == r["reference"] &&
                d->isSortable() == r["sortable"],
            "Inherited predicates: " + d->className);
      check(d->property("kind") == Value(Value::Class{r["kind"]}),
            "Inherited kind");
      check(d->property("visible") == Value(true) &&
                d->property("required") == Value(false),
            "Inherited defaults");
      check(d->property("shouldCacheDefault") ==
                Value(d->isRelation() && d->isMultiple()),
            "Inherited caching default");
      if (d->isReference()) {
        auto ref = std::make_shared<Description>();
        ref->id = ref->key = d->id + ".reference";
        d->reference = ref;
        exported.descriptions[ref->id] = ref;
      }
      if (d->isRelation())
        d->classes = {"String"};
      if (d->isOption())
        d->options = {Value("Ada"), Value("Bob")};
      if (r["base"] == "MASingleOptionDescription") {
        d->options = {Value(Value::Symbol{"Ada"}), Value(Value::Symbol{"Bob"})};
        d->labels = {{Value(Value::Symbol{"Ada"}), "Name"}};
        d->reference.lock()->className = "MASymbolDescription";
      };
      exported.descriptions[d->id] = d;
      root->children.push_back(d);
      auto sample = value(r["sample"]);
      check(validateValue(*d, sample).empty(), "Inherited validation");
      if (!r["invalid"].is_null()) {
        auto errors = validateValue(*d, value(r["invalid"]));
        check(errors.size() == 1 && errors[0].code == "MAKindError",
              "Inherited kind failure");
      }
      check(writeString(*d, sample) == r["output"], "Inherited writer");
      if (!r["input"].is_null())
        check(readString(*d, r["input"]) == sample, "Inherited reader");
      else
        fails([&] { readString(*d, "nonempty"); });
      Probe visitor;
      d->acceptMagritte(visitor);
      check(visitor.dispatch == r["visitor"], "Inherited visitor method");
      auto meta = descriptionModelFor(*d);
      for (const auto &key : r["metadataFields"])
        check(meta.descriptions.count(meta.root + "." + key.get<std::string>()),
              "Inherited schema field");
      if (r["base"] == "MABooleanDescription")
        check(!meta.descriptions.count(meta.root + ".required"),
              "Inherited annotation suppression");
      RecursiveDescriptionModel session;
      auto schema = session.describeReflective(d);
      auto model = session.bind(d);
      for (const auto &key : r["metadataFields"]) {
        auto field = session.resolver().descriptions.at(schema->id + "." +
                                                        key.get<std::string>());
        check(model->readUsing(*field) == d->property(key),
              "Inherited metadata selector");
      }
      if (!d->isContainer() && !d->isOption())
        check(meta.descriptions.at(meta.root + ".default")->className ==
                  d->className,
              "Metadata preserves self class construction");
      auto type = session.resolver().descriptions.at(schema->id + ".type");
      check(model->readUsing(*type) == Value(r["label"].get<std::string>()),
            "Actual registered class label");
      auto label = session.resolver().descriptions.at(schema->id + ".label");
      model->writeUsing(*label, Value("Edited"));
      check(d->property("label") == Value("Edited"),
            "Inherited metadata editing");
      auto batch = session.bootstrap(d);
      check(!batch.descriptions.empty(), "Inherited recursive bootstrap");
      auto copy = copyDescription(d, "copy." + d->id);
      check(copy.root->className == d->className &&
                descriptionEqual(*copy.root, *d),
            "Copy preserves species");
      auto base = *d;
      base.className = r["base"];
      check(!descriptionEqual(base, *d),
            "Inherited execution does not erase species equality");
    }
    auto number = exported.descriptions.at("InheritedNumberDescription");
    check(validateValue(*number, Value(std::int64_t(11)))[0].code ==
              "MARangeError",
          "Inherited numeric range");
    Predicate integer;
    integer.selector = "isInteger";
    number->portableConditions.push_back({"integer", integer});
    check(validateValue(*number, Value(5.5))[0].code == "MAConditionError",
          "Inherited portable conditions");
    number->portableConditions.clear();
    registry.registerClass(
        {"InheritedNumberLeaf", number->className, false, "Host number"});
    auto leaf = std::make_shared<Description>(*number);
    leaf->className = "InheritedNumberLeaf";
    leaf->properties["kind"] = Value(Value::Class{"Integer"});
    leaf->properties["min"] = Value(std::int64_t(5));
    check(validateValue(*leaf, Value(std::int64_t(4)))[0].code ==
              "MARangeError",
          "Explicit inherited property overrides");
    check(validateValue(*leaf, Value(5.5))[0].code == "MAKindError",
          "Explicit kind override");
    int calls = 0;
    leaf->defaultFunction = [&] {
      ++calls;
      return Value(std::int64_t(8));
    };
    check(leaf->defaultValue() == Value(std::int64_t(8)) && calls == 1,
          "Local callback override");
    auto localRoot = std::make_shared<Description>();
    localRoot->id = "local";
    localRoot->className = "MAContainer";
    localRoot->children = {leaf};
    Graph local;
    local.format = "magritte.graph/4";
    local.root = "local";
    local.descriptions = {{"local", localRoot}, {leaf->id, leaf}};
    fails([&] { local.encode(); });
    leaf->defaultFunction = {};
    check(leaf->readSlot("MASelectorAccessor", "type") == Value("Host number"),
          "Explicit class-side label override");
    auto priority = exported.descriptions.at("InheritedPriorityContainer");
    auto a = std::make_shared<Description>(),
         b = std::make_shared<Description>();
    a->id = a->key = "a";
    b->id = b->key = "b";
    a->className = b->className = "InheritedStringDescription";
    a->properties["priority"] = Value(std::int64_t(2));
    b->properties["priority"] = Value(std::int64_t(1));
    priority->setChildren({a, b});
    check(priority->orderedChildren()[0] == b, "Inherited priority ordering");
    DictionaryModel model;
    model.values = {{"a", Value("A")}, {"b", Value("B")}};
    check(validate(*priority, model).empty() &&
              writeString(*priority, model) == "B-A",
          "Inherited container interpreters");
    Graph containerGraph;
    containerGraph.format = "magritte.graph/4";
    containerGraph.root = priority->id;
    containerGraph.descriptions = {
        {priority->id, priority}, {"a", a}, {"b", b}};
    auto restoredContainer = Graph::decode(containerGraph.encode());
    check(
        restoredContainer.descriptions.at(restoredContainer.root)->className ==
            priority->className,
        "Registered container root import");
    CachedMemento memento(model, priority);
    memento.writeUsing(*a, Value("Changed"));
    memento.commit();
    check(model.values.at("a") == Value("Changed"),
          "Inherited container and field memento commit");
    priority->children.clear();
    auto password = exported.descriptions.at("InheritedPasswordDescription");
    check(password->obfuscated(Value("secret")) == "******" &&
              password->isObfuscated(Value("***")),
          "Inherited password protocol");
    auto boolean = exported.descriptions.at("InheritedBooleanDescription");
    check(boolean->labelForOption(Value(true)) == "Yep" &&
              boolean->allOptions().values().size() == 2,
          "Inherited Boolean options");
    registry.registerClasses(
        {{"UnimplementedDescription", "MAElementDescription"},
         {"NonDescription", "Object"}});
    auto names = registry.allClasses();
    for (const auto &name :
         {"UnknownDescription", "UnimplementedDescription", "NonDescription",
          "InheritedAbstractStringDescription"}) {
      Description bad;
      bad.className = name;
      fails([&] { validateValue(bad, Value()); });
      fails([&] { descriptionModelFor(bad); });
    }
    auto wire = exported.encode();
    auto imported = Graph::decode(wire);
    for (const auto &r : fixture["cases"]) {
      auto d = imported.descriptions.at(r["name"]);
      check(d->className == r["name"], "Wire preserves registered identity");
      check(validateValue(*d, value(r["sample"])).empty() &&
                writeString(*d, value(r["sample"])) == r["output"],
            "Imported inherited execution");
    }
    for (const auto &format :
         {"magritte.graph/1", "magritte.graph/2", "magritte.graph/3"}) {
      auto legacy = exported;
      legacy.format = format;
      fails([&] { legacy.encode(); });
    }
    auto unknown = J::parse(wire);
    unknown["descriptions"][0]["class"] = "WireInventedDescription";
    fails([&] { Graph::decode(unknown.dump()); });
    check(registry.allClasses() == names,
          "Import cannot register executable identities");
    if (argc > 1)
      std::ofstream(argv[1]) << wire;
    if (argc > 2) {
      std::ifstream in(argv[2]);
      std::string peer((std::istreambuf_iterator<char>(in)), {});
      auto replay = Graph::decode(peer);
      check(J::parse(replay.encode()) == J::parse(wire), "Peer graph replay");
    }
    std::cout << checks << " inheritance checks passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
