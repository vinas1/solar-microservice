import requests
import json 

# This is a work in progress script to export and import Zabbix dashboard configurations via the Zabbix API.

### ==========================================
### 1. SERVER & CONNECTION CONFIGURATION
### ==========================================

ZABBIX_URL = "http://your-zabbix-server-ip/zabbix/api_jsonrpc.php"
HEADERS = {"Content-Type": "application/json"} 

### ==========================================

### 2. CREDENTIALS (Choose ONE authentication method)

### ==========================================

### Method A: API Token (Recommended for Zabbix 5.4+)

API_TOKEN = "Your Zabbix API Token Here" 

### Method B: Legacy Username & Password (Fallback if token isn't generated)

ZABBIX_USER = "Admin"
ZABBIX_PASS = "zabbix" 

### ==========================================

### 3. DASHBOARD MANAGEMENT VARIABLES

### ==========================================

### List of dashboard names you want to export from Zabbix

DASHBOARDS_TO_GET = ["Morningstar", "Global view", "Zabbix server health"] 

### Configuration for dashboards you want to upload/deploy

### Format: {"local_json_file.json": "Desired Name in Zabbix UI"}

DASHBOARDS_TO_UPLOAD = {
"morningstar_dashboard.json": "Morningstar Production V2",
"global_view_dashboard.json": "Global Overview Backup Copy"
} 

### ==========================================

### API HELPER FUNCTIONS

### ==========================================

def get_auth_token():
"""Authenticates via user/password if API_TOKEN is empty."""
if API_TOKEN and API_TOKEN != "your_api_token_here":
return API_TOKEN 

print("[*] API_TOKEN not set. Attempting user/pass authentication...")
payload = {
"jsonrpc": "2.0",
"method": "user.login",
"params": {"user": ZABBIX_USER, "password": ZABBIX_PASS},
"id": 1
}
try:
response = requests.post(ZABBIX_URL, data=json.dumps(payload), headers=HEADERS)
result = response.json()
if "error" in result:
print(f"[-] Login Error: {result['error']['data']}")
return None
return result["result"]
except Exception as e:
print(f"[-] Authentication connection failed: {e}")
return None
def send_rpc(method, params, auth_token):
"""Sends JSON-RPC requests to the Zabbix API endpoint."""
payload = {
"jsonrpc": "2.0",
"method": method,
"params": params,
"auth": auth_token,
"id": 1
}
try:
response = requests.post(ZABBIX_URL, data=json.dumps(payload), headers=HEADERS)
response.raise_for_status()
result = response.json()
if "error" in result:
print(f"[-] Zabbix Error during {method}: {result['error']['data']}")
return None
return result["result"]
except requests.exceptions.RequestException as e:
print(f"[-] Connection Error: {e}")
return None 

def get_dashboard_code(dashboard_name, auth_token):
"""Retrieves a dashboard configuration array and saves it to a JSON file."""
print(f"[*] Fetching dashboard: {dashboard_name}")
params = {
"output": "extend",
"selectPages": "extend",
"filter": {"name": dashboard_name}
} 

result = send_rpc("dashboard.get", params, auth_token)
if result and len(result) > 0:# Extract the target dashboard configuration block
dash_blueprint = result[0]
filename = f"{dashboard_name.lower().replace(' ', '_')}_dashboard.json"
with open(filename, "w", encoding="utf-8") as f:
    json.dump(dash_blueprint, f, indent=4)
print(f"[+] Successfully saved code configuration to '{filename}'")
return True

else:
print(f"[-] Dashboard '{dashboard_name}' not found or pull failed.")
return False

def upload_new_dashboard(json_file_path, new_name, auth_token):
"""Uploads and creates a new dashboard from a stored JSON blueprint file."""
print(f"[*] Reading blueprint file: {json_file_path}")
try:
with open(json_file_path, "r", encoding="utf-8") as f:
dash_blueprint = json.load(f) 

### Build clean structural layout parameters for a fresh deployment

creation_params = {
"name": new_name,
"pages": dash_blueprint.get("pages", [])
} 

# Strip internal unique database indices to clear server placement conflicts

for page in creation_params["pages"]:
page.pop("dashboard_pageid", None)
for widget in page.get("widgets", []):
widget.pop("widgetid", None)

print(f"[*] Deploying clean dashboard to UI as: '{new_name}'")
result = send_rpc("dashboard.create", creation_params, auth_token)

if result and "dashboardids" in result:
print(f"[+] Success! New dashboard deployed with ID: {result['dashboardids'][0]}")
else:
print(f"[-] Dashboard deployment failed for '{new_name}'.")

except Exception as e:
print(f"[-] Failed to process or upload '{json_file_path}': {e}") 

### ==========================================

### MAIN EXECUTION ROUTINE

### ==========================================

if **name** == "**main**": 

### Step 1: Secure an active authentication token

token = get_auth_token() 

if token:
print("[+] Authentication Successful.\n") 

### Step 2: Loop and process all listed exports

print("=== EXPORTING DASHBOARDS ===")
for dash_name in DASHBOARDS_TO_GET:
get_dashboard_code(dash_name, token)
print("-" * 30) 

### Step 3: Loop and process all listed uploads

### (Uncomment the lines below when you are ready to upload code files)

### print("\n=== UPLOADING DASHBOARDS ===")

### for file_path, target_name in DASHBOARDS_TO_UPLOAD.items():

### upload_new_dashboard(file_path, target_name, token)

# print("-" * 30)

else:
print("[-] Aborting routine: Missing valid authentication credentials.")