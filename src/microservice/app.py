import subprocess
import logging
from fastapi import FastAPI, HTTPException, Request

app = FastAPI()
logging.basicConfig(level=logging.INFO)

ZABBIX_SERVER_IP = "gsdebian"  # Basement Zabbix VM IP
ZABBIX_HOST_NAME = "Solar-Service"      # Hostname configured in Zabbix

@app.post("/api/renogy")
async def receive_renogy(request: Request):
    payload = await request.json()
    if not payload:
        raise HTTPException(status_code=400, detail="Empty payload")

    sender_input = ""
    for controller, metrics in payload.items():
        if isinstance(metrics, dict):
            for key, val in metrics.items():
                item_key = f"{controller}.{key}"
                # REMOVED EXTRA QUOTES HERE:
                sender_input += f'{ZABBIX_HOST_NAME} {item_key} {val}\n'

    if sender_input:
        process = subprocess.Popen(
            ["zabbix_sender", "-z", ZABBIX_SERVER_IP, "-i", "-"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True
        )
        stdout, _ = process.communicate(input=sender_input)
        logging.info(f"Zabbix Output: {stdout.strip()}")

    return {"status": "ok"}
// written by: @vinas1 visit me on GitHub: vinas1.github.io or see the original project at: github.com/vinas1/esp32-bluetooth-collector