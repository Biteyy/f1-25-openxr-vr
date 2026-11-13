"""Builds settings_container.ui for the OpenXR VR addon's tile in the game's Settings menu.

The tile is a settings category (cat_openxr_vr, ui_command open_openxr_vr, see db/settings_tile.json). Opening it
needs an event in the Settings screen that routes to the addon's options page: t_openxr_vr -> Action.route
{key: open_openxr_vr}, the same recipe More Special Liveries uses for its AI Liveries tile. That addon replaces this
screen too, with its own t_ai_liveries event, so the screen built here carries both events: whichever addon loads
last, both tiles keep working (a route to an addon that is off is cancelled by Swofty).

Usage: python make_settings_container.py <Swofty tools dir with pkg.py> <stock settings_container.ui> <output .ui>
The stock screen is eaid://ui_package/screens/settings_container.ui from
2025_asset_groups/ui_package/screens/settings_container.erp (zstd-compressed fragment 0).
"""

import copy
import importlib.util
import os
import sys


def djb2(s):
    h = 5381
    for c in s.encode():
        h = (h * 33 + c) & 0xFFFFFFFF
    return h


# (event, route key) per tile: More Special Liveries' first, then this addon's
ROUTES = [("t_ai_liveries", "open_ai_liveries"), ("t_openxr_vr", "open_openxr_vr")]


def main(tools, stock_path, out_path):
    spec = importlib.util.spec_from_file_location("pkg", os.path.join(tools, "pkg.py"))
    pkg = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(pkg)
    Val, Obj = pkg.Val, pkg.Obj

    data = open(stock_path, "rb").read()
    p = pkg.Pkg(data)
    assert p.to_bytes() == data, "pkg.py does not round-trip the stock screen"

    # The screen's own event list: the one holding t_braking_point_recap (the stock screen's last event)
    lists = []

    def walk(n):
        if isinstance(n, pkg.Arr):
            if any(isinstance(e, Obj) and isinstance(e.get("eventName"), Val) and e["eventName"].value == "t_braking_point_recap"
                   for e in n.items):
                lists.append(n)
            for e in n.items:
                walk(e)
        elif isinstance(n, Obj):
            for _, v in n.items:
                walk(v)

    walk(p.root)
    assert len(lists) == 1, f"expected one screen event list, found {len(lists)}"
    events = lists[0]
    names = {e["eventName"].value for e in events.items}

    # Check the ids below against a stock signal to settings_container.Action
    def find_route_signal(n):
        if isinstance(n, Obj):
            if isinstance(n.get("to_object_name"), Val) and n["to_object_name"].value == "settings_container.Action":
                return n
            for _, v in n.items:
                r = find_route_signal(v)
                if r is not None:
                    return r
        elif isinstance(n, pkg.Arr):
            for e in n.items:
                r = find_route_signal(e)
                if r is not None:
                    return r
        return None

    action = find_route_signal(p.root)
    assert action is not None and action["to_object"].value == djb2("settings_container.Action")

    for event, key in ROUTES:
        if event in names:
            continue
        attributes = Obj([(Val("stri", "key"), Obj([(Val("stri", "value"), Val("stri", key))]))])
        signal = Obj([
            (Val("stri", "to_object"), Val("ui32", djb2("settings_container.Action"))),
            (Val("stri", "to_object_name"), Val("stri", "settings_container.Action")),
            (Val("stri", "to_pin_name"), Val("stri", "route")),
            (Val("stri", "to_pin"), Val("ui32", djb2("route"))),
            (Val("stri", "from_pin_name"), Val("stri", event)),
            (Val("stri", "attributes"), attributes),
        ])
        entry = copy.deepcopy(events.items[-1])  # same chunk versions as the stock events
        entry.set("eventName", Val("stri", event))
        entry.set("signals", pkg.Arr([signal], events.items[-1]["signals"].tag))
        entry.set("event", Val("ui32", djb2(event)))
        # Version-1 objects keep a hash per key; nested objects take the version of the stock ones
        signal.version = events.items[-1]["signals"].items[0].version
        attributes.version = signal.version
        attributes.items[0][1].version = signal.version
        events.items.append(entry)
        print("added", event, "->", key)

    out = p.to_bytes()
    check = pkg.Pkg(out)
    assert pkg.to_py(check.root) == pkg.to_py(p.root), "written screen does not read back the same"
    open(out_path, "wb").write(out)
    print("wrote", out_path, len(out), "bytes")


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    main(*sys.argv[1:])
