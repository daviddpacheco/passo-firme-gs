import csv
d = {}
for r in csv.DictReader(open("dados.csv")):
    d.setdefault(r["rotulo"], []).append(int(r["luminosidade"]))
for k, v in d.items():
    print(k, "n =", len(v), "min =", min(v), "max =", max(v), "media =", sum(v) // len(v))