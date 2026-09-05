#include "vehicle/FormulaXml.h"

#include <set>

namespace wizengine {
namespace vehicle {

xml::Element formulaElement(const FormulaGraphDesc& g) {
    xml::Element f("formula");
    f.set("name", g.name);
    for (const FormulaNodeDesc& n : g.nodes) {
        xml::Element e("node");
        e.setInt("id", n.id);
        e.set("type", n.kind);
        if (!n.name.empty()) e.set("name", n.name);
        if (!n.params.empty()) e.setNumbers("params", n.params.data(), n.params.size());
        const double pos[2] = {n.x, n.y};
        e.setNumbers("pos", pos, 2);
        f.append(std::move(e));
    }
    for (const FormulaWireDesc& w : g.wires) {
        xml::Element e("wire");
        e.setInt("from", w.from);
        if (w.fromPort != 0) e.setInt("fromPort", w.fromPort);
        e.setInt("to", w.to);
        if (w.port != 0) e.setInt("port", w.port);
        f.append(std::move(e));
    }
    return f;
}

FormulaGraphDesc formulaFromXml(const xml::Element& e,
                                std::vector<std::string>* warnings) {
    auto warn = [&](const std::string& m) {
        if (warnings) warnings->push_back(m);
    };
    FormulaGraphDesc g;
    g.name = e.attr("name");
    const std::string label = "<formula name=\"" + g.name + "\">";
    if (g.name.empty()) warn(label + ": needs a name");

    std::set<int> ids;
    int nextId = 1;
    for (const auto& c : e.children()) {
        if (c.name() == "node") {
            FormulaNodeDesc n;
            n.kind = c.attr("type");
            n.name = c.attr("name");
            if (!formulaKind(n.kind)) {
                warn(label + ": node type \"" + n.kind + "\" is unknown - ignored");
                continue;
            }
            n.id = c.integer("id", 0);
            if (n.id <= 0 || ids.count(n.id)) {
                // id 省略 / 重複は空き番号を採番（イベントノードと同じ流儀）。
                while (ids.count(nextId)) ++nextId;
                if (n.id > 0) {
                    warn(label + ": node id " + std::to_string(n.id) +
                         " is used twice - renumbered to " + std::to_string(nextId));
                }
                n.id = nextId;
            }
            ids.insert(n.id);
            if (n.id >= nextId) nextId = n.id + 1;
            double p[64];
            std::size_t count = c.numbers("params", p, 64);
            if (count == 0 && c.has("value")) {
                p[0] = c.number("value", 0.0);
                count = 1;
            }
            n.params.assign(p, p + count);
            double pos[2] = {0.0, 0.0};
            c.numbers("pos", pos, 2);
            n.x = pos[0];
            n.y = pos[1];
            g.nodes.push_back(std::move(n));
        } else if (c.name() == "wire") {
            FormulaWireDesc w;
            w.from = c.integer("from", 0);
            w.fromPort = c.integer("fromPort", 0);
            w.to = c.integer("to", 0);
            w.port = c.integer("port", 0);
            if (w.from <= 0 || w.to <= 0) {
                warn(label + ": <wire> needs from and to - ignored");
                continue;
            }
            g.wires.push_back(w);
        } else {
            warn(label + ": unsupported <" + c.name() + "> - ignored");
        }
    }
    return g;
}

}  // namespace vehicle
}  // namespace wizengine
