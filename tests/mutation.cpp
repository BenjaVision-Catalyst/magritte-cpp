#include <fstream>
#include <iostream>
#include <magritte/magritte.hpp>
#include <nlohmann/json.hpp>
using namespace magritte;
using J = nlohmann::json;
int checks = 0;
void check(bool b, const std::string &message = "Container mutation mismatch") {
  ++checks;
  if (!b)
    throw Error(message + " at check " + std::to_string(checks));
}
template <class F> void fails(F f) {
  bool caught = false;
  try {
    f();
  } catch (const Error &) {
    caught = true;
  }
  check(caught, "Expected failure");
}
std::shared_ptr<Description> field(const std::string &id,
                                   std::int64_t priority = 0) {
  auto d = std::make_shared<Description>();
  d->id = d->key = id;
  d->properties["priority"] = Value(priority);
  return d;
}
J ids(const Description &d) {
  J a = J::array();
  for (auto &c : d.orderedChildren())
    a.push_back(c->id);
  return a;
}
int main(int argc, char **argv) {
  try {
    J data;
    std::ifstream(std::string(FIXTURES) +
                  "/container-mutation-scenarios.json") >>
        data;
    auto &registry = ClassRegistry::global();
    registry.registerClasses(
        {{"MutatingContainer", "MAContainer"},
         {"MutatingPriorityContainer", "MAPriorityContainer"}});
    for (auto &row : data["cases"]) {
      Description container;
      container.id = "container";
      container.className = row["class"];
      std::map<std::string, std::shared_ptr<Description>> nodes;
      for (auto i = data["nodes"].begin(); i != data["nodes"].end(); ++i) {
        auto d = field(i.key(), i.value()["priority"]);
        d->key = i.value()["key"];
        nodes[i.key()] = d;
      }
      for (auto &step : row["steps"]) {
        auto operation = [&]() -> Description * {
          auto action = step["action"].get<std::string>();
          std::shared_ptr<Description> child;
          if (step.contains("child"))
            child = nodes.at(step["child"]);
          std::vector<std::shared_ptr<Description>> items;
          if (step.contains("children"))
            for (auto &id : step["children"])
              items.push_back(nodes.at(id));
          if (action == "set")
            return &container.setChildren(items);
          if (action == "add")
            return &container.add(child);
          if (action == "addAll")
            return &container.addAll(items);
          if (action == "remove")
            return &container.remove(*child);
          if (action == "moveUp")
            return &container.moveUp(*child);
          if (action == "moveDown")
            return &container.moveDown(*child);
          if (action == "resort")
            return &container.resort();
          if (action == "clear")
            return &container.removeAll();
          child->properties["priority"] =
              Value(step["value"].get<std::int64_t>());
          return &container;
        };
        if (step.value("error", false))
          fails(operation);
        else
          check(operation() == &container, "Mutators return the receiver");
        check(ids(container) == step["expected"],
              row["class"].get<std::string>() + " " +
                  step["action"].get<std::string>());
        check(ids(container) == step["expected"],
              "Repeated reads preserve backing state");
      }
    }
    auto a = field("a", 10), b = field("b", 20), c = field("c", 30);
    auto root = field("root");
    root->className = "MutatingPriorityContainer";
    root->setChildren({c, a, b});
    a->properties["priority"] = Value(std::int64_t(40));
    check(ids(*root) == J::array({"a", "b", "c"}));
    DictionaryModel values;
    values.values = {{"a", Value("A")}, {"b", Value("B")}, {"c", Value("C")}};
    check(writeString(*root, values) == "A-B-C" &&
          validate(*root, values).empty());
    auto copied = copyDescription(root, "copy");
    check(ids(*copied.root) == J::array({"b", "c", "a"}) &&
              ids(*root) == J::array({"a", "b", "c"}),
          "Priority postCopy re-converts children");
    auto selected =
        selectDescriptions(root, [](const auto &) { return true; }, "selected");
    check(ids(*selected.root) == J::array({"b", "c", "a"}));
    std::vector<std::string> trace;
    auto collected = collectDescriptions(
        root,
        [&](auto d) {
          trace.push_back(d->id);
          return d;
        },
        "collected");
    check(trace == std::vector<std::string>({"a", "b", "c"}) &&
          ids(*collected.root) == J::array({"b", "c", "a"}));
    auto range = copyRange(root, 0, 2, "range");
    check(ids(*range.root) == J::array({"b", "a"}));
    // Metadata setters use setChildren; a failing composition check restores
    // the exact old sequence.
    RecursiveDescriptionModel session;
    auto schema = session.describe(root);
    auto binding = session.bind(root);
    auto children =
        session.resolver().descriptions.at(schema->id + ".children");
    binding->writeUsing(*children,
                        Value(Value::Array{Value(Value::Reference{"c"}),
                                           Value(Value::Reference{"a"}),
                                           Value(Value::Reference{"b"})}));
    check(ids(*root) == J::array({"b", "c", "a"}));
    b->properties["priority"] = Value(std::int64_t(50));
    auto previous = ids(*root);
    fails([&] {
      binding->writeUsing(*children,
                          Value(Value::Array{Value(Value::Reference{"root"})}));
    });
    check(ids(*root) == previous);
    root->setChildren({a, b, c});
    b->properties["priority"] = Value(std::int64_t(5));
    root->add(c);
    check(ids(*root) == J::array({"c", "c", "a", "b"}),
          "Single insertion does not re-sort stale members");
    Graph g;
    g.format = "magritte.graph/4";
    g.root = "root";
    g.descriptions = {{"root", root}, {"a", a}, {"b", b}, {"c", c}};
    auto wire = g.encode();
    auto decoded = Graph::decode(wire);
    check(ids(*decoded.descriptions.at("root")) == ids(*root),
          "Stored stale/duplicate order survives import");
    auto legacy = J::parse(wire);
    for (auto &d : legacy["descriptions"])
      d.erase("childrenOrder");
    auto normalized = Graph::decode(legacy.dump());
    check(ids(*normalized.descriptions.at("root")) ==
          J::array({"b", "c", "c", "a"}));
    auto canonical = field("canonical");
    canonical->className = "MAPriorityContainer";
    canonical->children = root->children;
    auto old = g;
    old.root = "canonical";
    old.descriptions.erase("root");
    old.descriptions["canonical"] = canonical;
    old.format = "magritte.graph/3";
    fails([&] { old.encode(); });
    auto malformed = J::parse(wire);
    for (auto &d : malformed["descriptions"])
      if (d["id"] == "a")
        d["childrenOrder"] = "stored";
    fails([&] { Graph::decode(malformed.dump()); });
    // Comparison failure on set/resort is before replacing parent storage; bulk
    // add retains appended membership.
    auto bad = field("bad");
    bad->properties["priority"] = Value("bad");
    Description p;
    p.className = "MAPriorityContainer";
    p.setChildren({a, c});
    auto before = ids(p);
    fails([&] { p.setChildren({a, bad}); });
    check(ids(p) == before);
    fails([&] { p.add(bad); });
    check(ids(p) == before);
    fails([&] { p.addAll({b, bad}); });
    check(p.children.size() == 4 && p.children.back() == bad);
    fails([&] { p.resort(); });
    check(p.children.size() == 4);
    Description plain;
    plain.className = "MAContainer";
    fails([&] { plain.resort(); });
    Description scalar;
    fails([&] { scalar.add(a); });
    Limits low;
    low.items = 0;
    fails([&] { plain.add(a, low); });
    check(plain.children.empty());
    plain.setChildren({a, b});
    plain.addAll(plain.children);
    check(plain.children.size() == 4, "Input arrays are native snapshots");
    if (argc > 1)
      std::ofstream(argv[1]) << wire;
    if (argc > 2) {
      std::ifstream in(argv[2]);
      std::string peer((std::istreambuf_iterator<char>(in)), {});
      check(J::parse(Graph::decode(peer).encode()) == J::parse(wire),
            "Cross-port replay");
    }
    std::cout << checks << " container mutation checks passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
