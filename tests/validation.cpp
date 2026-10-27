#include <fstream>
#include <iostream>
#include <magritte/magritte.hpp>
#include <nlohmann/json.hpp>
using namespace magritte;
using J = nlohmann::json;
int checks = 0;
void check(bool ok, const std::string &s) {
  ++checks;
  if (!ok)
    throw Error(s);
}
template <class F> void fails(F f) {
  bool caught = false;
  try {
    f();
  } catch (const Error &) {
    caught = true;
  }
  check(caught, "Expected operational validation failure");
}
J read(const char *name) {
  J j;
  std::ifstream(std::string(FIXTURES) + "/" + name) >> j;
  return j;
}
std::shared_ptr<Description> d(std::string id,
                               std::string cls = "MAStringDescription") {
  auto p = std::make_shared<Description>();
  p->id = p->key = id;
  p->className = cls;
  p->properties["label"] = Value(id);
  return p;
}
J node(const std::shared_ptr<ValidationIssue> &e, const Graph &g) {
  if (!e)
    return nullptr;
  check(g.descriptions.at(e->tag->id).get() == e->tag,
        "Error retains actual graph tag");
  check(e->kindOf("MAValidationError") && e->kindOf("MAError") &&
            e->isResumable(),
        "Error hierarchy/resumability");
  J children = J::array();
  for (auto &c : e->collection)
    children.push_back(node(c, g));
  return J{{"class", e->code},
           {"description", e->tag->id},
           {"signalText", e->signalText ? J(*e->signalText) : J(nullptr)},
           {"collection", children},
           {"isResumable", e->isResumable()}};
}
ValidationResult run(Graph &g, const J &entry) {
  if (entry.contains("object"))
    return validateDetailed(*g.descriptions.at(g.root),
                            g.objects.at(entry["object"]), {}, &g);
  return validateValueDetailed(*g.descriptions.at(entry["description"]),
                               decodeValue(entry["value"].dump()), &g);
}
J report(const ValidationResult &r, const Graph &g) {
  J flat = J::array();
  for (const auto &e : r.flatErrors())
    flat.push_back(J{{"description", e.descriptionId},
                     {"code", e.code},
                     {"message", e.message}});
  return J{{"error", node(r.error, g)},
           {"flat", flat},
           {"print", r.error ? r.error->printString() : ""}};
}
J replay(const J &fixture) {
  J output = fixture;
  for (auto &c : output["cases"]) {
    auto g = Graph::decode(c["graph"].dump());
    auto result = run(g, c["entry"]);
    check(report(result, g) == c["expected"], c["name"].get<std::string>());
    auto clone = Graph::decode(g.encode());
    check(report(run(clone, c["entry"]), clone) == c["expected"],
          "Graph roundtrip validation");
    c["graph"] = J::parse(g.encode());
  }
  return output;
}
int main(int argc, char **argv) {
  try {
    auto fixture = read("validation-protocol-scenarios.json");
    for (auto &c : fixture["registrations"])
      ClassRegistry::global().registerClass({c["name"], c["superclass"]});
    auto exported = replay(fixture);
    if (argc > 1)
      std::ofstream(argv[1]) << exported.dump(2);
    if (argc > 2) {
      J peer;
      std::ifstream(argv[2]) >> peer;
      (void)replay(peer);
    }
    auto field = d("field");
    ValidationContext context(*field);
    bool resumed = false, skipped = false, passed = false;
    std::shared_ptr<ValidationIssue> signaled;
    context.tryValidation(
        [&](auto &c) {
          signaled = c.signal("MAKindError", {});
          resumed = true;
        },
        [&](auto &) { skipped = true; });
    context.tryValidation([](auto &) {}, [&](auto &) { passed = true; });
    auto result = context.finish();
    check(resumed && !skipped && passed, "Native continuation protocol");
    check(result.error->collection[0] == signaled &&
              signaled->signalText == std::nullopt &&
              signaled->messageText().empty(),
          "Signal identity/raw nil/computed text");
    try {
      result.raiseIfInvalid();
      check(false, "Expected typed validation failure");
    } catch (const ValidationFailure &e) {
      check(e.error == result.error, "Typed failure retains exact object");
    }
    field->properties["label"] = Value("Changed");
    check(result.error->printString() == "Changed: ",
          "Error printing reads current tag label");
    check(result.error->signalText == std::optional<std::string>("field"),
          "Aggregate signal is the original label");
    field->conditions.push_back(
        {std::nullopt, [](const Value &) { return false; }});
    auto nilCondition = validateValueDetailed(*field, Value("ok"));
    check(!nilCondition.error->collection[0]->signalText &&
              nilCondition.flatErrors()[0].message.empty(),
          "Nil local condition label");
    field->conditions.clear();
    ClassRegistry::global().registerClass(
        {"ValidationLeaf", "MAValidationError"});
    ValidationContext custom(*field);
    custom.signal("ValidationLeaf", "custom");
    check(custom.finish().error->collection[0]->kindOf("MAError"),
          "Runtime error class registration");
    fails([&] { context.signal("Object", "bad"); });
    std::weak_ptr<Description> weak;
    ValidationResult retained;
    {
      auto owned = d("owned");
      weak = owned;
      retained = validateValueDetailed(*owned, Value(true));
    }
    check(!weak.expired(), "Shared tag retained after graph/caller release");
    retained = {};
    check(weak.expired(), "Tag releases with native error owner");
    auto schema = d("schema", "MAContainer"),
         number = d("number", "MANumberDescription");
    schema->setChildren({number});
    number->properties["min"] = Value(7LL);
    number->setRangeErrorMessage(Value("range"));
    struct State {
      Value value = Value(3LL);
      int reads = 0, conditions = 0, modelConditions = 0;
      bool absent = false;
    };
    State state;
    NativeModel model(&state);
    number->nativeRead = [](const void *p) {
      auto &s = *const_cast<State *>(static_cast<const State *>(p));
      ++s.reads;
      return s.value;
    };
    number->conditions.push_back({"number condition", [&](const Value &) {
                                    ++state.conditions;
                                    return false;
                                  }});
    schema->conditions.push_back(
        Condition::onModel("model condition", [&](const Model &m) {
          check(&m == &model, "Actual native model condition target");
          ++state.modelConditions;
          return false;
        }));
    Predicate predicate;
    predicate.selector = "notEmpty";
    schema->portableConditions.push_back({"portable model", predicate});
    model.readers["MASelectorAccessor:notEmpty"] = [](const void *) {
      return Value(true);
    };
    model.binaryReaders["="] = [&](const void *, const Value &, const Graph *) {
      return Value(state.absent);
    };
    auto native = validateDetailed(*schema, model);
    auto leaves = native.flatErrors();
    check(leaves.size() == 3 && leaves[0].descriptionId == "schema" &&
              leaves[1].code == "MARangeError" &&
              leaves[2].code == "MAConditionError",
          "Container before child, range before conditions");
    check(state.modelConditions == 1 && state.reads == 1 &&
              state.conditions == 1,
          "Callbacks execute once");
    model.readers["MASelectorAccessor:notEmpty"] = [](const void *) {
      return Value("not Boolean");
    };
    fails([&] { validateDetailed(*schema, model); });
    model.readers["MASelectorAccessor:notEmpty"] = [](const void *) {
      return Value(true);
    };
    number->properties["readOnly"] = Value(true);
    auto savedReads = state.reads;
    check(validateDetailed(*number, model).passed() &&
              state.reads == savedReads,
          "Scalar model entry gate avoids reading");
    number->properties.erase("readOnly");
    number->properties["visible"] = Value(false);
    (void)validateDetailed(*schema, model);
    check(state.reads == 2 && state.conditions == 1,
          "Parent reads child before child's visibility gate");
    schema->properties["readOnly"] = Value(true);
    check(validateDetailed(*schema, model).passed() && state.reads == 2,
          "Container gate skips all reads");
    schema->properties.erase("readOnly");
    number->properties.erase("visible");
    schema->properties["kind"] = Value(Value::Class{"String"});
    auto before = state.modelConditions;
    check(validateDetailed(*schema, model).flatErrors().size() == 3 &&
              state.modelConditions == before && state.reads == 3,
          "Container kind failure still visits children");
    schema->properties.erase("kind");
    state.absent = true;
    before = state.modelConditions;
    check(validateDetailed(*schema, model).flatErrors().size() == 2 &&
              state.modelConditions == before && state.reads == 4,
          "Container undefined return still visits children");
    state.absent = false;
    schema->conditions.clear();
    schema->portableConditions[0].predicate.index = 1;
    schema->portableConditions[0].predicate.arguments = {Value(true)};
    schema->portableConditions[0].predicate.selector = "=";
    fails([&] { validateDetailed(*schema, model); });
    schema->portableConditions.clear();
    schema->conditions.push_back(Condition::onModel(
        "throws", [](const Model &) -> bool { throw Error("host failure"); }));
    fails([&] { validateDetailed(*schema, model); });
    schema->conditions.clear();
    check(!isSatisfiedBy(*number, Value(3LL)) &&
              isSatisfiedBy(*field, Value("ok")),
          "Satisfaction projection");
    Limits tiny;
    tiny.items = 1;
    fails([&] { validateValueDetailed(*number, Value(3LL), nullptr, tiny); });
    tiny = Limits{};
    tiny.depth = 0;
    fails([&] { result.flatErrors(tiny); });
    tiny = Limits{};
    tiny.bytes = 1;
    fails([&] { result.error->printString(tiny); });
    auto editSchema = d("edit", "MAContainer"), data = d("data");
    editSchema->setChildren({data});
    AdaptiveModel actual;
    actual.description = editSchema;
    actual.values["data"] = Value("old");
    CheckedMemento editor(actual, editSchema);
    int editorChecks = 0;
    editSchema->conditions.push_back(
        Condition::onModel("editor", [&](const Model &m) {
          check(&m == &editor,
                "Memento is the actual container condition target");
          ++editorChecks;
          return true;
        }));
    editor.writeUsing(*data, Value("local"));
    actual.values["data"] = Value("remote");
    editSchema->setConflictErrorMessage(Value());
    auto conflict = editor.validateDetailed();
    check(editorChecks == 0, "Conflict path skips ordinary validation");
    check(conflict.error && conflict.error->code == "MAConflictError" &&
              conflict.error->tag == editSchema.get() &&
              !conflict.error->signalText &&
              conflict.flatErrors()[0].message.empty(),
          "Memento emits direct conflict with nil message");
    check(editor.readUsing(*data) == Value("remote") && !editor.hasChanges(),
          "Conflict resets before emission");
    data->properties["required"] = Value(true);
    editor.writeUsing(*data, Value());
    auto regular = editor.validateDetailed();
    check(editorChecks == 1,
          "Regular memento path validates container conditions");
    check(regular.error->code == "MAMultipleErrors" &&
              regular.error->collection[0]->tag == data.get(),
          "Memento consumes nested regular validation");
    std::cout << checks << " validation protocol checks passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "check " << checks << ": " << e.what() << '\n';
    return 1;
  }
}
