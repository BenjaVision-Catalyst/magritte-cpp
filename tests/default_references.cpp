#include <fstream>
#include <iostream>
#include <magritte/magritte.hpp>
#include <nlohmann/json.hpp>
using namespace magritte;
using J = nlohmann::json;
int checks = 0;
void check(bool value,
           const std::string &message = "Default reference mismatch") {
  ++checks;
  if (!value)
    throw Error(message + " at check " + std::to_string(checks));
}
template <class F> void fails(F f) {
  bool failed = false;
  try {
    f();
  } catch (const Error &) {
    failed = true;
  }
  check(failed, "Expected refusal");
}
std::shared_ptr<Description>
node(const std::string &id, const std::string &cls = "MAStringDescription") {
  auto d = std::make_shared<Description>();
  d->id = d->key = id;
  d->className = cls;
  return d;
}
struct CustomContainer : Description {
  mutable int calls = 0;
  std::shared_ptr<Description> descriptionContainer() const override {
    ++calls;
    return node("custom.container", "MAContainer");
  }
};
int main(int argc, char **argv) {
  try {
    J data;
    std::ifstream(std::string(FIXTURES) +
                  "/default-reference-scenarios.json") >>
        data;
    auto &registry = ClassRegistry::global();
    for (auto &r : data["cases"])
      if (r["base"] != r["className"])
        registry.registerClass({r["className"], r["base"], false, "Child"});
    for (auto &r : data["classes"])
      registry.registerClass(
          {r["name"], r["superclass"], false, r["label"].get<std::string>()});
    for (auto &r : data["cases"]) {
      auto d = node("owner", r["className"]);
      check(d->reference.expired());
      if (r["self"].get<bool>()) {
        d->display("asString");
        auto self = d->referenceDescription();
        check(self == d && d->reference.expired());
        {
          auto copy = copyDescription(d, "boolean.copy");
          check(copy.root->referenceDescription() == copy.root);
        }
        std::weak_ptr<Description> weak = d;
        d.reset();
        check(!weak.expired());
        self.reset();
        check(weak.expired());
        continue;
      }
      auto copy = copyDescription(d, "uninitialized.copy");
      check(d->reference.expired() && copy.root->reference.expired(),
            "Copy must preserve lazy nil");
      d->display("asString");
      auto ref = d->referenceDescription();
      check(ref->className == "MAStringDescription" && !ref->id.empty() &&
            ref->id != d->id);
      check(d->referenceDescription() == ref &&
            ref->property("displayBlockOrSymbol") ==
                Value(Value::Symbol{"asString"}));
      d->setReference(ref);
      std::weak_ptr<Description> weak = ref;
      ref.reset();
      check(!weak.expired(), "Owner retains generated default");
      auto original = weak.lock();
      original->properties["label"] = Value("Kept");
      auto materialized = copyDescription(d, "materialized.copy");
      auto copied = materialized.root;
      auto child = copied->reference.lock();
      check(child && child != original &&
            child->property("label") == Value("Kept"));
      materialized = {};
      check(copied->reference.lock() == child,
            "Copied generated default retains ownership");
      auto isolated = copy.root->referenceDescription();
      check(isolated != original && isolated->id != original->id);
      auto old = original;
      d->setReference(nullptr);
      check(d->reference.expired());
      auto fresh = d->referenceDescription();
      check(fresh != old && fresh->id != old->id);
      old.reset();
      original.reset();
      check(weak.expired(), "Reset releases owned default");
      auto explicitRef = node("explicit");
      d->setReference(explicitRef);
      check(d->referenceDescription() == explicitRef);
      explicitRef.reset();
      fails([&] { d->referenceDescription(); });
      d->setReference(nullptr);
      if (d->isOption()) {
        d->setUndefined(Value("missing"));
        check(d->referenceDescription()->property("undefined") ==
              Value("missing"));
      }
    }
    Description stackBoolean;
    stackBoolean.className = "MABooleanDescription";
    check(stackBoolean.referenceDescription().get() == &stackBoolean);
    Description unsupported;
    fails([&] { unsupported.referenceDescription(); });
    for (auto &r : data["relations"]) {
      auto relation = node("relation", "MAToManyRelationDescription");
      relation->classes = r["classes"].get<std::vector<std::string>>();
      auto before = relation->classes;
      check(relation->allClasses() ==
                r["ordered"].get<std::vector<std::string>>() &&
            relation->classes == before);
      auto common = relation->commonClass();
      if (r["common"].is_null()) {
        auto container = std::get<std::shared_ptr<Description>>(common);
        check(container->className == "MAPriorityContainer" &&
              container->children.empty());
        check(container->property("label") == Value("1:m Relation"));
        check(std::get<std::shared_ptr<Description>>(relation->commonClass()) !=
              container);
      } else
        check(std::get<Value::Class>(common).name == r["common"]);
      check(relation->referenceDescription()->className ==
                "MAStringDescription",
            "Relation getter uses superclass String default, not a class "
            "template");
    }
    CustomContainer custom;
    custom.className = "MAToOneRelationDescription";
    check(std::get<std::shared_ptr<Description>>(custom.commonClass())->id ==
              "custom.container" &&
          custom.calls == 1);
    auto relation = node("bad", "MAToOneRelationDescription");
    relation->classes = {"UnknownRefClass"};
    fails([&] { relation->allClasses(); });
    fails([&] { relation->commonClass(); });
    auto option = node("option", "DefaultChildSingleOptionDescription");
    option->options = {Value("Ada")};
    check(validateValue(*option, Value("Ada")).empty() &&
          option->reference.expired());
    option->properties["extensible"] = Value(true);
    check(validateValue(*option, Value("new")).empty() &&
          !option->reference.expired());
    auto token = node("token", "MATokenDescription");
    check(readString(*token, "a,b") ==
          Value(Value::Array{Value("a"), Value("b")}));
    // Recursive metadata nil writes reset state and subsequent gets bind the
    // fresh default.
    RecursiveDescriptionModel session;
    auto target = node("target", "MAToOneRelationDescription");
    auto schema = session.describeReflective(target);
    auto binding = session.bind(target);
    auto field = session.resolver().descriptions.at(schema->id + ".reference");
    check(binding->respondsToSelector("allClasses") &&
          binding->readSlot("MASelectorAccessor", "allClasses") ==
              Value(Value::Array{}));
    auto containerValue =
        binding->readSlot("MASelectorAccessor", "commonClass");
    check(containerValue.data.index() == 5);
    auto booleanBinding =
        session.bind(node("bound.boolean", "MABooleanDescription"));
    check(booleanBinding->readSlot("MASelectorAccessor", "reference") ==
          Value(Value::Reference{"bound.boolean"}));
    auto first = binding->readUsing(*field);
    check(!target->reference.expired());
    binding->writeUsing(*field, Value());
    check(target->reference.expired());
    auto second = binding->readUsing(*field);
    check(first != second && session.resolver().nativeObjects.count(
                                 std::get<Value::Reference>(second.data).id));
    // Stable export covers both nil storage and an explicitly registered
    // generated default.
    Graph g;
    g.root = "root";
    g.format = "magritte.graph/4";
    auto root = node("root", "MAContainer");
    auto unset = node("unset", "MAToOneRelationDescription");
    unset->classes = {"String"};
    auto selected = node("selected", "DefaultChildSingleOptionDescription");
    selected->options = {Value("Ada")};
    auto generated = selected->referenceDescription();
    generated->id = "generated";
    Accessor null;
    null.className = "MANullAccessor";
    generated->accessor = null;
    auto flag = node("flag", "DefaultChildBooleanDescription");
    check(flag->referenceDescription() == flag);
    root->children = {unset, selected, flag};
    g.descriptions = {{"root", root},
                      {"unset", unset},
                      {"selected", selected},
                      {"generated", generated},
                      {"flag", flag}};
    auto wire = g.encode();
    check(unset->reference.expired() &&
          J::parse(wire)["descriptions"].size() == 5);
    auto decoded = Graph::decode(wire);
    check(decoded.descriptions.at("unset")->reference.expired());
    check(decoded.descriptions.at("unset")->referenceDescription()->className ==
          "MAStringDescription");
    auto nilCopy = copyDescription(unset, "nil.copy");
    check(unset->reference.expired() && nilCopy.root->reference.expired());
    for (auto format :
         {"magritte.graph/1", "magritte.graph/2", "magritte.graph/3"}) {
      auto legacy = g;
      legacy.format = format;
      fails([&] { legacy.encode(); });
    }
    auto missing = g;
    missing.descriptions.erase("generated");
    fails([&] { missing.encode(); });
    Graph canonical;
    canonical.root = "canonical";
    canonical.format = "magritte.graph/4";
    auto plain = node("canonical", "MAContainer");
    plain->children = {unset};
    canonical.descriptions = {{"canonical", plain}, {"unset", unset}};
    auto nullable = J::parse(canonical.encode());
    nullable["format"] = "magritte.graph/3";
    fails([&] { Graph::decode(nullable.dump()); });
    relation->classes = {"RefLeft", "RefRight"};
    Limits low;
    low.items = 1;
    fails([&] { relation->allClasses(low); });
    low = Limits{};
    low.bytes = 1;
    fails([&] { relation->allClasses(low); });
    auto callback = node("callback", "MASingleOptionDescription");
    callback->display(std::make_shared<DisplayBlock>([]() { return "local"; }));
    plain->children = {callback};
    canonical.descriptions = {{"canonical", plain}, {"callback", callback}};
    fails([&] { canonical.encode(); });
    check(callback->reference.expired(),
          "Export must not execute a lazy getter");
    auto propagated = callback->referenceDescription();
    check(propagated->displayBlock == callback->displayBlock);
    if (argc > 1)
      std::ofstream(argv[1]) << wire;
    if (argc > 2) {
      std::ifstream in(argv[2]);
      std::string peer((std::istreambuf_iterator<char>(in)), {});
      check(J::parse(Graph::decode(peer).encode()) == J::parse(wire),
            "Cross-port replay");
    }
    std::cout << checks << " default reference checks passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
