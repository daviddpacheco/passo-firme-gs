"""
Passo Firme - IA no PC (CitizenTech)
Uso:
  python robo_ia.py coletar clara      (60 leituras com luz boa)
  python robo_ia.py coletar penumbra
  python robo_ia.py coletar escura
  python robo_ia.py rodar              (treina com dados.csv e opera o robo)

Controle no modo 'rodar' (Windows): W=frente  S=re  A=esquerda  D=direita  ESPACO=parar  Q=sair
"""
import sys, os, json, csv, time
from collections import deque
from datetime import datetime

import serial                      # pip install pyserial
import pyttsx3                     # pip install pyttsx3
from sklearn.tree import DecisionTreeClassifier  # pip install scikit-learn

try:
    import msvcrt                  # teclado no Windows
except ImportError:
    msvcrt = None

PORTA = "COM6"                     # <- troque pela porta COM de SAIDA do Bluetooth do PC
BAUD = 115200
ARQ_DADOS = "dados.csv"
ARQ_RELATORIO = "relatorio_pontos_escuros.csv"

AVISOS = {
    "clara": "Iluminacao adequada. Caminho seguro.",
    "penumbra": "Atencao. Trecho com pouca iluminacao a frente. Caminhe devagar.",
    "escura": "Alerta. Trecho muito escuro e com risco de queda. Parei o carrinho. Procure ajuda ou um caminho iluminado.",
}


def abrir_serial():
    return serial.Serial(PORTA, BAUD, timeout=1)


def ler_json(ser):
    linha = ser.readline().decode(errors="ignore").strip()
    if not linha.startswith("{"):
        return None
    try:
        return json.loads(linha)
    except json.JSONDecodeError:
        return None


def falar(texto):
    print("[VOZ]", texto)
    try:
        engine = pyttsx3.init()
        engine.say(texto)
        engine.runAndWait()
    except Exception as e:
        print("[VOZ] falhou:", e)


def gerar_aviso(classe, lum, delta):
    """Usa um LLM se houver ANTHROPIC_API_KEY; senao usa o texto pronto."""
    if os.getenv("ANTHROPIC_API_KEY"):
        try:
            import anthropic       # pip install anthropic
            client = anthropic.Anthropic()
            msg = client.messages.create(
                model="claude-haiku-5-5",
                max_tokens=80,
                messages=[{"role": "user", "content":
                    f"Voce e um robo assistente de idosos e pessoas com baixa visao. "
                    f"Iluminacao classificada como '{classe}' (leitura {lum}, variacao {delta}). "
                    f"Gere UM aviso falado curto (max 25 palavras), em portugues, sem emojis."}],
            )
            return msg.content[0].text.strip()
        except Exception as e:
            print("[LLM] falhou, usando texto padrao:", e)
    return AVISOS[classe]


# ---------- COLETA ----------
def coletar(rotulo, n=60):
    ser = abrir_serial()
    hist = deque(maxlen=4)
    novo = not os.path.exists(ARQ_DADOS)
    count = 0
    print(f"Coletando {n} leituras '{rotulo}'... varie um pouco a luz dentro dessa condicao.")
    with open(ARQ_DADOS, "a", newline="") as f:
        w = csv.writer(f)
        if novo:
            w.writerow(["luminosidade", "delta", "rotulo"])
        while count < n:
            d = ler_json(ser)
            if not d or "luminosidade" not in d:
                continue
            lum = d["luminosidade"]
            delta = lum - hist[0] if hist else 0
            hist.append(lum)
            w.writerow([lum, delta, rotulo])
            count += 1
            print(count, lum, delta)
    ser.close()
    print("Pronto.")


# ---------- TREINO ----------
def treinar():
    X, y = [], []
    with open(ARQ_DADOS) as f:
        for r in csv.DictReader(f):
            X.append([int(r["luminosidade"]), int(r["delta"])])
            y.append(r["rotulo"])
    modelo = DecisionTreeClassifier(max_depth=3, random_state=0).fit(X, y)
    print(f"Modelo treinado com {len(X)} amostras. Classes: {list(modelo.classes_)}")
    return modelo


# ---------- OPERACAO ----------
def tecla_para_comando(k):
    return {"w": b"F", "s": b"B", "a": b"L", "d": b"R", " ": b"S"}.get(k)


def rodar():
    modelo = treinar()
    ser = abrir_serial()
    hist = deque(maxlen=4)
    classe_atual, candidata, cont = None, None, 0
    novo = not os.path.exists(ARQ_RELATORIO)
    rel = open(ARQ_RELATORIO, "a", newline="")
    wr = csv.writer(rel)
    if novo:
        wr.writerow(["data_hora", "luminosidade", "classe"])
    print("Rodando. Controle: W/A/S/D, ESPACO=parar, Q=sair")

    try:
        while True:
            if msvcrt and msvcrt.kbhit():
                k = msvcrt.getwch().lower()
                if k == "q":
                    break
                cmd = tecla_para_comando(k)
                if cmd:
                    ser.write(cmd)

            d = ler_json(ser)
            if not d:
                continue
            if "alerta" in d:
                print("[ROBO]", d["alerta"])
                continue

            lum = d["luminosidade"]
            delta = lum - hist[0] if hist else 0
            hist.append(lum)
            pred = modelo.predict([[lum, delta]])[0]
            print(f"lum={lum} delta={delta} -> {pred} | dist={d.get('distancia_cm')}cm")

            # confirma a classe por 3 leituras seguidas (evita falso alarme)
            if pred == candidata:
                cont += 1
            else:
                candidata, cont = pred, 1

            if cont >= 3 and pred != classe_atual:
                classe_atual = pred
                if pred == "escura":
                    ser.write(b"S")  # decisao autonoma: para o carrinho
                    wr.writerow([datetime.now().isoformat(timespec="seconds"), lum, pred])
                    rel.flush()
                falar(gerar_aviso(pred, lum, delta))
                ser.reset_input_buffer()  # descarta leituras acumuladas durante a fala
    finally:
        ser.write(b"S")
        ser.close()
        rel.close()


if __name__ == "__main__":
    if len(sys.argv) >= 3 and sys.argv[1] == "coletar":
        coletar(sys.argv[2])
    elif len(sys.argv) >= 2 and sys.argv[1] == "rodar":
        rodar()
    else:
        print(__doc__)
