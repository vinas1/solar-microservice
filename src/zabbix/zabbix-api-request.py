import requests
import json
import os
import yaml

# ============================================================================
# CONFIGURATION (All variables for easy editing)
# ============================================================================

# 1. SERVER & CONNECTION CONFIGURATION
ZABBIX_URL = "http://gsdebian/zabbix/api_jsonrpc.php"  # e.g., "https://zabbix.example.com/zabbix/api_jsonrpc.php"
HEADERS = {"Content-Type": "application/json"}

# 2. CREDENTIALS (Can be overridden by secrets.yaml)
ZABBIX_API_TOKEN = None  # Leave empty or set to your actual token
ZABBIX_USER = "Admin"    # Default username if no API token
ZABBIX_PASS = "zabbix"   # Default password if no API token

# 3. DASHBOARD MANAGEMENT VARIABLES
DASHBOARDS_TO_GET = ["Morningstar", "Global view", "Zabbix server health"]

### Configuration for dashboards you want to upload/deploy
### Format: {"local_json_file.json": "Desired Name in Zabbix UI"}
DASHBOARDS_TO_UPLOAD = {
    "morningstar_renogy_dashboard.json": "Solar Production V2",
    "morningstar_renogy_dashboard.json": "Solar Production V3",
}

UPLOAD_FILE_PATH = None  # Set to a specific file path if you want to override the default upload list


# ============================================================================
# CONFIGURATION LOADER
# ============================================================================

def load_config(config_path="secrets.yaml"):
    """Load configuration from a YAML file.
    
    Args:
        config_path: Path to the YAML configuration file (default: secrets.yaml)
        
    Returns:
        dict: Configuration dictionary
        
    Raises:
        FileNotFoundError: If the config file doesn't exist
        yaml.YAMLError: If the YAML file is malformed
    """
    if not os.path.exists(config_path):
        print(f"[-] Configuration file '{config_path}' not found.")
        raise FileNotFoundError(f"Configuration file '{config_path}' not found.")
    
    try:
        with open(config_path, "r", encoding="utf-8") as f:
            config = yaml.safe_load(f)
            
        if config is None:
            print("[-] Configuration file is empty or invalid.")
            raise ValueError("Configuration file is empty or invalid.")
            
        return config
        
    except yaml.YAMLError as e:
        print(f"[-] YAML parsing error in '{config_path}': {e}")
        raise
    except Exception as e:
        print(f"[-] Error reading configuration file '{config_path}': {e}")
        raise


# ============================================================================
# 1. SERVER & CONNECTION CONFIGURATION (Overridden by secrets.yaml if available)
# ============================================================================

try:
    config = load_config()
    ZABBIX_URL = config.get("zabbix", {}).get("url", ZABBIX_URL)
except Exception as e:
    print(f"[-] Failed to load configuration: {e}")


# ============================================================================
# 2. CREDENTIALS (Overridden by secrets.yaml if available)
# ============================================================================

try:
    config = load_config()
    ZABBIX_API_TOKEN = config.get("zabbix", {}).get("api_token") or ZABBIX_API_TOKEN
except Exception as e:
    print(f"[-] Failed to load API token: {e}")


try:
    config = load_config()
    ZABBIX_USER = config.get("zabbix", {}).get("username", "Admin") if config else "Admin"
except Exception as e:
    print(f"[-] Failed to load username: {e}")


try:
    config = load_config()
    ZABBIX_PASS = config.get("zabbix", {}).get("password", "zabbix") if config else "zabbix"
except Exception as e:
    print(f"[-] Failed to load password: {e}")


# ============================================================================
# API HELPER FUNCTIONS
# ============================================================================

def get_auth_token():
    """Authenticates via API Token if available, otherwise falls back to user/password."""
    
    if ZABBIX_API_TOKEN and ZABBIX_API_TOKEN != "your_api_token_here":
        print("[+] Using API Token authentication from configuration.")
        return ZABBIX_API_TOKEN
    
    print("[*] API Token not found or invalid. Attempting user/pass authentication...")
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
    
    if result and len(result) > 0:
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
        
        # Fix: Iterate through ALL pages and clean each page + its widgets
        all_pages = dash_blueprint.get("pages", [])
        for page in all_pages:
            page.pop("dashboard_pageid", None)
            for widget in page.get("widgets", []):
                widget.pop("widgetid", None)
        
        creation_params = {
            "name": new_name,
            "pages": dash_blueprint.get("pages", [])
        }
        
        print(f"[*] Deploying clean dashboard to UI as: '{new_name}'")
        result = send_rpc("dashboard.create", creation_params, auth_token)

        if result and "dashboardids" in result:
            print(f"[+] Success! New dashboard deployed with ID: {result['dashboardids'][0]}")
        else:
            print(f"[-] Dashboard deployment failed for '{new_name}'.")
            
    except Exception as e:
        print(f"[-] Failed to process or upload '{json_file_path}': {e}")


# ============================================================================
### MAIN EXECUTION ROUTINE
# ============================================================================

if __name__ == "__main__":
    
    token = get_auth_token()
    
    if token:
        print("[+] Authentication Successful.\n")
        
        print("=== EXPORTING DASHBOARDS ===")
        for dash_name in DASHBOARDS_TO_GET:
            get_dashboard_code(dash_name, token)
            print("-" * 30)
            
        ### (Uncomment the lines below when you are ready to upload code files)
        
        print("\n=== UPLOADING DASHBOARDS ===")
        for file_path, target_name in DASHBOARDS_TO_UPLOAD.items():
            upload_new_dashboard(file_path, target_name, token)
            
        # print("-" * 30)
        
    else:
        print("[-] Aborting routine: Missing valid authentication credentials.")