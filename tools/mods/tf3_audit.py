"""Create a review queue for TF3-only cards from the locally extracted games.

The report contains game text and must remain in gen/. A donor is a research lead,
not proof of equivalent behaviour: per-card checks, costs and triggers can differ.
Usage: python -I tools/mods/tf3_audit.py [output.json]
"""
import json
import os
import sys
from collections import Counter

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools", "game"))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mapa_cartas
from tf3_legacy_effects import EFEITO


def main():
    path = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.join(ROOT, "gen", "tf3_audit.json")
    gen = os.path.realpath(os.path.join(ROOT, "gen"))
    if os.path.commonpath([gen, os.path.realpath(path)]) != gen:
        raise SystemExit("The report contains game data: output must be inside gen/.")
    base = os.path.dirname(os.path.abspath(__file__))
    known = set(EFEITO)
    for name in ("tf3_efeitos.json", "tf3_efeitos_auto.json", "tf3_stats.json", "tf3_stats_auto.json"):
        with open(os.path.join(base, name), encoding="utf-8") as f:
            known.update(map(int, json.load(f)))
    m = mapa_cartas.montar()
    ids = set(mapa_cartas.ids_novos(m))
    rows = []
    for c in m["cards"]:
        if c["tf3_id"] not in ids:
            continue
        status = "has_rules_review_needed" if c["tf3_id"] in known else "normal" if c["category"] == 0 else "no_rules"
        rows.append({"id": c["tf3_id"], "name": c["name"], "kind": c["kind"], "status": status,
                     "text": c["text_tf3"], "record_status": c["effect_status"], "donor_candidate": c["donor"]})
    rows.sort(key=lambda r: (r["status"] != "no_rules", -(r["donor_candidate"] or {}).get("similarity", 0), r["id"]))
    with open(path, "w", encoding="utf-8") as f:
        json.dump({"counts": dict(Counter(r["status"] for r in rows)), "cards": rows}, f, ensure_ascii=False, indent=1)
    print("Review queue:", len(rows), "cards;", dict(Counter(r["status"] for r in rows)))
    print("Donors are candidates, not verified effects. Report:", path)


if __name__ == "__main__":
    main()
