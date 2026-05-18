"""
=====================================================================
 Smart Hydroponic Farming System – Flask Backend
 Author  : Hydro-AI System
 File    : app.py
 Purpose : Receive sensor data & images from ESP32/ESP32-CAM,
           call Gemini Vision API for disease detection,
           serve dashboard via REST API,
           manage plant profiles & dynamic thresholds
=====================================================================
"""

import os
import io
import json
import base64
import logging
import datetime
import psycopg2
import psycopg2.extras
from pathlib import Path
from threading import Lock
from dotenv import load_dotenv
from flask import Flask, request, jsonify, send_from_directory
from flask_cors import CORS
import google.genai as genai
from google.genai import types as genai_types
from PIL import Image

# ─── Load environment variables ───────────────────────────────────
# override=True ensures a changed .env replaces any older shell value.
load_dotenv(override=True)

# ─── Configuration ────────────────────────────────────────────────
GEMINI_API_KEY  = os.getenv("GEMINI_API_KEY", "AIzaSyDjfbGkSr1YIgUNdmXjM60NrGT1xzGkvOA")
GEMINI_MODEL_ID = "gemini-2.0-flash"
UPLOAD_DIR      = Path("uploads")
FRONTEND_DIR    = Path("../frontend")
MAX_IMAGE_SIZE  = 5 * 1024 * 1024   # 5 MB guard
DATABASE_URL     = os.getenv("postgresql://postgres.kwshvgramwulumyzwwfu:oqdHLQfWkdO6pkpc@aws-1-ap-southeast-1.pooler.supabase.com:5432/postgres", "")  # Supabase PostgreSQL connection string

# ─── Logging ──────────────────────────────────────────────────────
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(name)s – %(message)s",
    datefmt="%Y-%m-%d %H:%M:%S",
)
log = logging.getLogger("HydroAI")

# ─── Flask App ────────────────────────────────────────────────────
app = Flask(__name__, static_folder=str(FRONTEND_DIR), static_url_path="")
CORS(app)  # Allow cross-origin requests from the dashboard

UPLOAD_DIR.mkdir(exist_ok=True)

# ═══════════════════════════════════════════════════════════════════
#  PLANT DATABASE
#  Each plant has: temperature_min/max, humidity_min/max,
#                  ph_min/max, tds_min/max
#  Units: temp=°C, humidity=%, ph=0–14, tds=ppm
# ═══════════════════════════════════════════════════════════════════

DEFAULT_PLANTS = {
    "lettuce": {
        "display_name": "Lettuce",
        "emoji": "🥬",
        "temperature_min": 18, "temperature_max": 24,
        "humidity_min": 50,    "humidity_max": 70,
        "ph_min": 5.5,         "ph_max": 6.5,
        "tds_min": 500,        "tds_max": 800,
    },
    "tomato": {
        "display_name": "Tomato",
        "emoji": "🍅",
        "temperature_min": 20, "temperature_max": 28,
        "humidity_min": 60,    "humidity_max": 80,
        "ph_min": 5.8,         "ph_max": 6.8,
        "tds_min": 700,        "tds_max": 1000,
    },
    "basil": {
        "display_name": "Basil",
        "emoji": "🌿",
        "temperature_min": 22, "temperature_max": 30,
        "humidity_min": 55,    "humidity_max": 75,
        "ph_min": 5.5,         "ph_max": 6.5,
        "tds_min": 700,        "tds_max": 1120,
    },
    "spinach": {
        "display_name": "Spinach",
        "emoji": "🍃",
        "temperature_min": 15, "temperature_max": 22,
        "humidity_min": 50,    "humidity_max": 70,
        "ph_min": 6.0,         "ph_max": 7.0,
        "tds_min": 1260,       "tds_max": 1610,
    },
    "strawberry": {
        "display_name": "Strawberry",
        "emoji": "🍓",
        "temperature_min": 18, "temperature_max": 26,
        "humidity_min": 60,    "humidity_max": 80,
        "ph_min": 5.5,         "ph_max": 6.5,
        "tds_min": 1260,       "tds_max": 1540,
    },
    "mint": {
        "display_name": "Mint",
        "emoji": "🌱",
        "temperature_min": 18, "temperature_max": 25,
        "humidity_min": 55,    "humidity_max": 70,
        "ph_min": 5.5,         "ph_max": 6.0,
        "tds_min": 1400,       "tds_max": 1680,
    },
}

def _get_conn():
    """Open a new psycopg2 connection to Supabase. Uses RealDictCursor."""
    url = DATABASE_URL
    if not url:
        raise RuntimeError("DATABASE_URL env var is not set")
    # Ensure SSL is required (Supabase mandates it)
    if "sslmode" not in url:
        url += "?sslmode=require"
    conn = psycopg2.connect(url, cursor_factory=psycopg2.extras.RealDictCursor)
    conn.autocommit = False
    return conn


def init_db():
    """Create all tables in Supabase if they don't already exist."""
    conn = _get_conn()
    try:
        cur = conn.cursor()
        cur.execute("""
            CREATE TABLE IF NOT EXISTS sensor_readings (
                id                SERIAL PRIMARY KEY,
                timestamp         TEXT    NOT NULL,
                air_temperature   REAL,
                humidity          REAL,
                water_temperature REAL,
                ph                REAL,
                tds               REAL,
                water_level       INTEGER,
                sunlight          INTEGER,
                pump              INTEGER,
                light             INTEGER,
                mist              INTEGER,
                shed              INTEGER
            );
        """)
        cur.execute("CREATE INDEX IF NOT EXISTS idx_timestamp ON sensor_readings(timestamp);")
        cur.execute("""
            CREATE TABLE IF NOT EXISTS tank_config (
                id               INTEGER PRIMARY KEY,
                tank_height_cm   REAL NOT NULL DEFAULT 30,
                tank_width_cm    REAL NOT NULL DEFAULT 30,
                tank_length_cm   REAL NOT NULL DEFAULT 30,
                sensor_offset_cm REAL NOT NULL DEFAULT 0,
                updated_at       TEXT NOT NULL
            );
        """)
        cur.execute("""
            CREATE TABLE IF NOT EXISTS kv_store (
                key   TEXT PRIMARY KEY,
                value TEXT NOT NULL
            );
        """)
        cur.execute("""
            CREATE TABLE IF NOT EXISTS custom_plants (
                plant_key TEXT PRIMARY KEY,
                data      TEXT NOT NULL
            );
        """)
        conn.commit()

        # Seed tank_config row if absent
        cur.execute("SELECT COUNT(*) AS cnt FROM tank_config WHERE id=1")
        if cur.fetchone()["cnt"] == 0:
            now = datetime.datetime.utcnow().isoformat() + "Z"
            cur.execute(
                "INSERT INTO tank_config (id, tank_height_cm, tank_width_cm, tank_length_cm, sensor_offset_cm, updated_at) VALUES (1,%s,%s,%s,%s,%s)",
                (30, 30, 30, 0, now)
            )
            conn.commit()

        log.info("[DB] Supabase PostgreSQL tables ready")
    except Exception as e:
        conn.rollback()
        log.error(f"[DB] init_db failed: {e}")
    finally:
        conn.close()


def _pg_insert_reading(sensor: dict, ts: str):
    """Persist one sensor snapshot to Supabase sensor_readings."""
    try:
        conn = _get_conn()
        cur = conn.cursor()
        cur.execute("""
            INSERT INTO sensor_readings
                (timestamp, air_temperature, humidity, water_temperature,
                 ph, tds, water_level, sunlight, pump, light, mist, shed)
            VALUES (%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s)
        """, (
            ts,
            sensor.get("air_temperature"),
            sensor.get("humidity"),
            sensor.get("water_temperature"),
            sensor.get("ph"),
            sensor.get("tds"),
            sensor.get("water_level"),
            sensor.get("sunlight"),
            int(bool(sensor.get("pump"))),
            int(bool(sensor.get("light"))),
            int(bool(sensor.get("mist"))),
            int(bool(sensor.get("shed"))),
        ))
        conn.commit()
        conn.close()
    except Exception as e:
        log.warning(f"[DB] insert_reading failed: {e}")


# ─── KV helpers (settings stored as key/value rows) ──────────────────────
def _kv_get(key: str, default=None):
    try:
        conn = _get_conn()
        cur = conn.cursor()
        cur.execute("SELECT value FROM kv_store WHERE key=%s", (key,))
        row = cur.fetchone()
        conn.close()
        return json.loads(row["value"]) if row else default
    except Exception as e:
        log.warning(f"[KV] get({key}) failed: {e}")
        return default

def _kv_set(key: str, value):
    try:
        conn = _get_conn()
        cur = conn.cursor()
        cur.execute(
            "INSERT INTO kv_store(key,value) VALUES(%s,%s) ON CONFLICT(key) DO UPDATE SET value=EXCLUDED.value",
            (key, json.dumps(value))
        )
        conn.commit()
        conn.close()
    except Exception as e:
        log.warning(f"[KV] set({key}) failed: {e}")


# ─── Custom plants helpers ────────────────────────────────────────────────
def load_plants_from_db() -> dict:
    try:
        conn = _get_conn()
        cur = conn.cursor()
        cur.execute("SELECT plant_key, data FROM custom_plants")
        rows = cur.fetchall()
        conn.close()
        result = {}
        for row in rows:
            try:
                result[row["plant_key"]] = json.loads(row["data"])
            except Exception:
                pass
        if result:
            log.info(f"[PLANTS] Loaded {len(result)} custom plant(s) from Supabase")
        return result
    except Exception as e:
        log.warning(f"[PLANTS] DB load failed: {e}")
        return {}

def save_plant_to_db(key: str, plant: dict):
    try:
        conn = _get_conn()
        cur = conn.cursor()
        cur.execute(
            "INSERT INTO custom_plants(plant_key,data) VALUES(%s,%s) ON CONFLICT(plant_key) DO UPDATE SET data=EXCLUDED.data",
            (key, json.dumps(plant))
        )
        conn.commit()
        conn.close()
    except Exception as e:
        log.warning(f"[PLANTS] DB save({key}) failed: {e}")

def delete_plant_from_db(key: str):
    try:
        conn = _get_conn()
        cur = conn.cursor()
        cur.execute("DELETE FROM custom_plants WHERE plant_key=%s", (key,))
        conn.commit()
        conn.close()
    except Exception as e:
        log.warning(f"[PLANTS] DB delete({key}) failed: {e}")


# ─── Dashboard settings (stored in kv_store) ─────────────────────────────
DEFAULT_SETTINGS = {
    "current_plant": "lettuce",
    "active_chart_tab": "env",
    "refresh_interval_ms": 2000,
    "notes": "",
    "auto_mode": True,
}

def load_settings() -> dict:
    try:
        saved = _kv_get("dashboard_settings", {})
        merged = {**DEFAULT_SETTINGS, **saved}
        log.info(f"[SETTINGS] Loaded from Supabase — plant: {merged.get('current_plant')}, auto_mode: {merged.get('auto_mode')}")
        return merged
    except Exception as e:
        log.warning(f"[SETTINGS] load failed: {e}")
        return dict(DEFAULT_SETTINGS)

def save_settings(settings: dict):
    try:
        _kv_set("dashboard_settings", settings)
        log.info(f"[SETTINGS] Saved to Supabase — plant: {settings.get('current_plant')}, auto_mode: {settings.get('auto_mode')}")
    except Exception as e:
        log.warning(f"[SETTINGS] save failed: {e}")


# Initialise DB tables, then load persisted state
init_db()
_saved_settings = load_settings()
dashboard_settings: dict = _saved_settings

# ─── Shared In-Memory State ───────────────────────────────────────
state_lock = Lock()

# Currently selected plant — restored from DB on boot
current_plant_key: str = dashboard_settings.get("current_plant", "lettuce")

# Plant database: defaults merged with DB-stored customisations
_custom_plants = load_plants_from_db()
plant_db: dict = {**DEFAULT_PLANTS, **_custom_plants}

# Rolling plant alert messages
MAX_ALERTS = 20
plant_alerts: list = []

latest_sensor_data: dict = {
    "air_temperature":   0.0,
    "humidity":          0.0,
    "water_temperature": 0.0,
    "ph":                7.0,
    "tds":               0.0,
    "water_level":       0,
    "sunlight":          0,
    "pump":              False,
    "light":             False,
    "mist":              False,
    "shed":              False,
    # Autonomous reason strings (populated by ESP32)
    "pump_reason":       "Waiting for ESP32…",
    "light_reason":      "Waiting for ESP32…",
    "mist_reason":       "Waiting for ESP32…",
    "shed_reason":       "Waiting for ESP32…",
    # Thresholds reported by ESP32
    "shed_close_threshold": 70,
    "light_on_threshold":   40,
    "humidity_min":         45,
    "humidity_max":         65,
    "timestamp":         None,
}

latest_ai_result: dict = {
    "disease_name":    "No data yet",
    "confidence":      0,
    "recommendation":  "Waiting for first plant image…",
    "raw_response":    "",
    "analysis_time":   None,
    "status":          "pending",   # pending | healthy | diseased | error
}

pending_commands: dict = {
    "pump":  None,
    "light": None,
    "mist":  None,
    "shed":  None,
}

# ─── Sensor History Buffer ────────────────────────────────────────
from collections import deque

HISTORY_MAX = 180   # keep last 180 readings (6 min at 2 s intervals)

sensor_history: deque = deque(maxlen=HISTORY_MAX)  # each entry = snapshot dict

# ─── Gemini Client Setup ─────────────────────────────────────────
_GEMINI_CLIENT_KEY = None
GEMINI_CLIENT = None


def get_gemini_client():
    """Return a Gemini client that always matches the current API key."""
    global GEMINI_CLIENT, _GEMINI_CLIENT_KEY, GEMINI_API_KEY

    load_dotenv(override=True)
    GEMINI_API_KEY = os.getenv("GEMINI_API_KEY", "").strip()
    if not GEMINI_API_KEY or GEMINI_API_KEY == "YOUR_GEMINI_API_KEY_HERE":
        raise RuntimeError("GEMINI_API_KEY is missing or still set to the placeholder value")

    if GEMINI_CLIENT is None or _GEMINI_CLIENT_KEY != GEMINI_API_KEY:
        GEMINI_CLIENT = genai.Client(api_key=GEMINI_API_KEY)
        _GEMINI_CLIENT_KEY = GEMINI_API_KEY
        log.info("[GEMINI] Client refreshed from current GEMINI_API_KEY")

    return GEMINI_CLIENT

GEMINI_GENERATION_CONFIG = genai_types.GenerateContentConfig(
    temperature=0.2,        # Low temperature for factual/precise answers
    top_p=0.85,
    max_output_tokens=512,
    safety_settings=[
        genai_types.SafetySetting(category="HARM_CATEGORY_HARASSMENT",        threshold="BLOCK_NONE"),
        genai_types.SafetySetting(category="HARM_CATEGORY_HATE_SPEECH",       threshold="BLOCK_NONE"),
        genai_types.SafetySetting(category="HARM_CATEGORY_SEXUALLY_EXPLICIT", threshold="BLOCK_NONE"),
        genai_types.SafetySetting(category="HARM_CATEGORY_DANGEROUS_CONTENT", threshold="BLOCK_NONE"),
    ]
)

# ─────────────────────────────────────────────────────────────────
#  GEMINI DISEASE DETECTION
# ─────────────────────────────────────────────────────────────────
DETECTION_PROMPT = """
You are an expert plant pathologist AI specialized in hydroponic crop diseases.
Analyze the provided plant leaf/plant image and return ONLY a JSON object with these exact keys:

{
  "disease_name": "<name of disease or 'Healthy' if no disease>",
  "confidence": <integer 0-100 representing confidence percentage>,
  "recommendation": "<1-2 sentence actionable treatment or care recommendation>"
}

Analysis rules:
1. Be specific – use scientific/common disease names (e.g., "Powdery Mildew (Erysiphe cichoracearum)", "Bacterial Leaf Spot", "Iron Deficiency Chlorosis")
2. If the plant appears healthy, set disease_name to "Healthy"
3. Consider discoloration, spots, lesions, wilting, chlorosis, necrosis, or mold
4. Confidence must reflect image quality and symptom clarity
5. Recommendation must be actionable and relevant to hydroponics
6. Return ONLY the JSON object – no markdown, no backticks, no extra text
"""


def analyze_image_with_gemini(image_bytes: bytes) -> dict:
    """
    Send image bytes to Gemini Vision for plant disease analysis.
    Returns a structured dict with disease_name, confidence, recommendation.
    """
    try:
        client = get_gemini_client()

        # Convert raw bytes to PIL Image for dimension logging
        image_pil = Image.open(io.BytesIO(image_bytes))
        if image_pil.mode not in ("RGB", "L"):
            image_pil = image_pil.convert("RGB")

        log.info(f"[GEMINI] Sending image to Gemini Vision ({image_pil.width}×{image_pil.height})…")

        # Build inline image part from original bytes
        image_part = genai_types.Part.from_bytes(
            data=image_bytes,
            mime_type="image/jpeg",
        )

        # Call Gemini multimodal API (new SDK)
        response = client.models.generate_content(
            model=GEMINI_MODEL_ID,
            contents=[DETECTION_PROMPT, image_part],
            config=GEMINI_GENERATION_CONFIG,
        )

        raw_text = response.text.strip() if response.text else ""
        log.info(f"[GEMINI] Raw response: {raw_text[:300]}")

        # Parse JSON from response
        result = parse_gemini_response(raw_text)
        result["raw_response"]  = raw_text
        result["analysis_time"] = datetime.datetime.utcnow().isoformat() + "Z"
        result["status"]        = "healthy" if result["disease_name"].lower() == "healthy" else "diseased"
        return result

    except Exception as e:
        log.error(f"[GEMINI] Unexpected error: {e}", exc_info=True)
        return _error_result(f"Analysis error: {str(e)[:100]}")


def parse_gemini_response(text: str) -> dict:
    """
    Safely extract JSON from Gemini reply.
    Falls back to heuristic parsing if direct parse fails.
    """
    import re

    # Attempt 1: Direct JSON parse
    try:
        data = json.loads(text)
        return _validate_result(data)
    except json.JSONDecodeError:
        pass

    # Attempt 2: Extract JSON block from surrounding text
    json_match = re.search(r'\{[^{}]+\}', text, re.DOTALL)
    if json_match:
        try:
            data = json.loads(json_match.group())
            return _validate_result(data)
        except json.JSONDecodeError:
            pass

    # Attempt 3: Heuristic line extraction
    disease_name   = "Unknown"
    confidence     = 50
    recommendation = "Please inspect plant manually."

    for line in text.splitlines():
        line_lower = line.lower()
        if "disease" in line_lower or "name" in line_lower:
            parts = line.split(":", 1)
            if len(parts) > 1:
                disease_name = parts[1].strip().strip('"').strip("'")
        elif "confidence" in line_lower:
            import re as _re
            nums = _re.findall(r'\d+', line)
            if nums:
                confidence = min(int(nums[0]), 100)
        elif any(k in line_lower for k in ["recommend", "treat", "suggestion"]):
            parts = line.split(":", 1)
            if len(parts) > 1:
                recommendation = parts[1].strip().strip('"').strip("'")

    log.warning("[GEMINI] Used heuristic parsing fallback")
    return {
        "disease_name":   disease_name,
        "confidence":     confidence,
        "recommendation": recommendation,
        "status":         "diseased" if disease_name.lower() not in ("healthy", "unknown") else "pending",
    }


def _validate_result(data: dict) -> dict:
    """Ensure required keys exist and types are correct."""
    return {
        "disease_name":   str(data.get("disease_name", "Unknown")),
        "confidence":     max(0, min(100, int(data.get("confidence", 50)))),
        "recommendation": str(data.get("recommendation", "No recommendation available.")),
    }


def _error_result(message: str) -> dict:
    return {
        "disease_name":   "Analysis Error",
        "confidence":     0,
        "recommendation": message,
        "raw_response":   message,
        "analysis_time":  datetime.datetime.utcnow().isoformat() + "Z",
        "status":         "error",
    }


# ─────────────────────────────────────────────────────────────────
#  PLANT ALERT HELPER
#  Compare sensor data against current plant conditions and
#  generate human-readable alert messages.
# ─────────────────────────────────────────────────────────────────
def check_plant_alerts(sensor: dict, plant: dict, plant_name: str) -> list:
    """Return list of alert dicts for out-of-range readings."""
    alerts = []
    ts = datetime.datetime.utcnow().isoformat() + "Z"

    checks = [
        ("air_temperature", plant.get("temperature_min"), plant.get("temperature_max"),
         "Temperature", "°C", "🌡️"),
        ("humidity",        plant.get("humidity_min"),    plant.get("humidity_max"),
         "Humidity",    "%",  "💧"),
        ("ph",              plant.get("ph_min"),          plant.get("ph_max"),
         "pH",          "",   "⚗️"),
        ("tds",             plant.get("tds_min"),         plant.get("tds_max"),
         "TDS",         " ppm","🧪"),
    ]

    for key, lo, hi, label, unit, icon in checks:
        val = sensor.get(key)
        if val is None or lo is None or hi is None:
            continue
        if val < lo:
            alerts.append({
                "level":   "warning",
                "icon":    icon,
                "message": f"{label} too low ({val:.1f}{unit}) for {plant_name} — ideal: {lo}–{hi}{unit}",
                "time":    ts,
            })
        elif val > hi:
            alerts.append({
                "level":   "warning",
                "icon":    icon,
                "message": f"{label} too high ({val:.1f}{unit}) for {plant_name} — ideal: {lo}–{hi}{unit}",
                "time":    ts,
            })

    return alerts


# ─────────────────────────────────────────────────────────────────
#  ROUTES – Plant Management
# ─────────────────────────────────────────────────────────────────

@app.route("/plants", methods=["GET"])
def get_plants():
    """Return all available plant profiles."""
    with state_lock:
        db_copy = dict(plant_db)
        selected = current_plant_key
    return jsonify({
        "plants":   db_copy,
        "selected": selected,
    }), 200


@app.route("/set-plant", methods=["POST"])
def set_plant():
    """
    Select the active plant.
    Body: {"plant": "tomato"}
    """
    global current_plant_key
    try:
        data = request.get_json(force=True, silent=True)
        if not data or "plant" not in data:
            return jsonify({"error": "Missing 'plant' key"}), 400

        key = data["plant"].strip().lower()
        with state_lock:
            if key not in plant_db:
                return jsonify({"error": f"Unknown plant '{key}'. Use /plants to list available plants."}), 404
            current_plant_key = key
            plant = plant_db[key]
            # Persist the selection so it survives server restarts
            dashboard_settings["current_plant"] = key
            save_settings(dashboard_settings)

        log.info(f"[PLANT] Active plant set to: {key}")
        return jsonify({"status": "ok", "selected": key, "conditions": plant}), 200

    except Exception as e:
        log.error(f"[PLANT] set-plant error: {e}", exc_info=True)
        return jsonify({"error": str(e)}), 500


@app.route("/current-plant", methods=["GET"])
def get_current_plant():
    """
    Return current plant name + conditions.
    ESP32 calls this to fetch dynamic thresholds.
    """
    with state_lock:
        key   = current_plant_key
        plant = dict(plant_db.get(key, {}))
    return jsonify({
        "plant":      key,
        "conditions": plant,
    }), 200


@app.route("/add-plant", methods=["POST"])
def add_plant():
    """
    Add or update a custom plant profile.
    Body: {
      "key": "cucumber",
      "display_name": "Cucumber",
      "emoji": "🥒",
      "temperature_min": 22, "temperature_max": 28,
      "humidity_min": 65,    "humidity_max": 80,
      "ph_min": 5.5,         "ph_max": 6.5,
      "tds_min": 1050,       "tds_max": 1750
    }
    """
    global plant_db
    try:
        data = request.get_json(force=True, silent=True)
        if not data:
            return jsonify({"error": "Invalid JSON"}), 400

        key = data.get("key", "").strip().lower()
        if not key:
            return jsonify({"error": "Missing 'key' field for plant identifier"}), 400

        required = ["temperature_min","temperature_max","humidity_min","humidity_max",
                    "ph_min","ph_max","tds_min","tds_max"]
        missing = [f for f in required if f not in data]
        if missing:
            return jsonify({"error": f"Missing fields: {missing}"}), 400

        plant_entry = {
            "display_name":    data.get("display_name", key.title()),
            "emoji":           data.get("emoji", "🌱"),
            "temperature_min": float(data["temperature_min"]),
            "temperature_max": float(data["temperature_max"]),
            "humidity_min":    float(data["humidity_min"]),
            "humidity_max":    float(data["humidity_max"]),
            "ph_min":          float(data["ph_min"]),
            "ph_max":          float(data["ph_max"]),
            "tds_min":         float(data["tds_min"]),
            "tds_max":         float(data["tds_max"]),
            # Optional extended thresholds (use sensible defaults if not provided)
            "water_temp_min":       float(data.get("water_temp_min", 18)),
            "water_temp_max":       float(data.get("water_temp_max", 26)),
            "water_level_warn":     int(data.get("water_level_warn",  50)),
            "water_level_crit":     int(data.get("water_level_crit",  25)),
            "light_on_threshold":   int(data.get("light_on_threshold",  40)),
            "light_off_threshold":  int(data.get("light_off_threshold", 55)),
            "shed_close_threshold": int(data.get("shed_close_threshold",70)),
            "shed_open_threshold":  int(data.get("shed_open_threshold", 50)),
        }

        with state_lock:
            plant_db[key] = plant_entry
            # Persist to DB (custom plants only)
            save_plant_to_db(key, plant_entry)

        log.info(f"[PLANT] Added/updated custom plant: {key}")
        return jsonify({"status": "ok", "key": key, "plant": plant_entry}), 200

    except Exception as e:
        log.error(f"[PLANT] add-plant error: {e}", exc_info=True)
        return jsonify({"error": str(e)}), 500


@app.route("/update-plant", methods=["POST"])
def update_plant():
    """
    Edit the conditions of an existing plant (including defaults).
    Body: {"key": "lettuce", "temperature_max": 26, ...}
    Partial updates supported – only provided fields are overwritten.
    """
    global plant_db
    try:
        data = request.get_json(force=True, silent=True)
        if not data:
            return jsonify({"error": "Invalid JSON"}), 400

        key = data.get("key", "").strip().lower()
        if not key:
            return jsonify({"error": "Missing 'key' field"}), 400

        with state_lock:
            if key not in plant_db:
                return jsonify({"error": f"Plant '{key}' not found"}), 404

            editable_float = [
                "temperature_min","temperature_max",
                "humidity_min","humidity_max",
                "water_temp_min","water_temp_max",
                "ph_min","ph_max",
                "tds_min","tds_max",
            ]
            editable_int = [
                "water_level_warn","water_level_crit",
                "light_on_threshold","light_off_threshold",
                "shed_close_threshold","shed_open_threshold",
            ]
            editable_str = ["display_name", "emoji"]
            for field in editable_float:
                if field in data: plant_db[key][field] = float(data[field])
            for field in editable_int:
                if field in data: plant_db[key][field] = int(data[field])
            for field in editable_str:
                if field in data: plant_db[key][field] = str(data[field])

            # Persist to DB (save modified plant — includes modified defaults)
            save_plant_to_db(key, dict(plant_db[key]))
            updated = dict(plant_db[key])

        log.info(f"[PLANT] Updated conditions for: {key}")
        return jsonify({"status": "ok", "key": key, "plant": updated}), 200

    except Exception as e:
        log.error(f"[PLANT] update-plant error: {e}", exc_info=True)
        return jsonify({"error": str(e)}), 500


@app.route("/delete-plant", methods=["POST"])
def delete_plant():
    """
    Delete a custom plant. Default plants cannot be deleted.
    Body: {"key": "myplant"}
    """
    global plant_db, current_plant_key
    try:
        data = request.get_json(force=True, silent=True)
        key  = (data or {}).get("key", "").strip().lower()
        if not key:
            return jsonify({"error": "Missing 'key'"}), 400

        with state_lock:
            if key in DEFAULT_PLANTS:
                return jsonify({"error": "Cannot delete built-in plants"}), 403
            if key not in plant_db:
                return jsonify({"error": f"Plant '{key}' not found"}), 404
            del plant_db[key]
            if current_plant_key == key:
                current_plant_key = "lettuce"
            delete_plant_from_db(key)

        log.info(f"[PLANT] Deleted custom plant: {key}")
        return jsonify({"status": "ok", "deleted": key}), 200

    except Exception as e:
        return jsonify({"error": str(e)}), 500


@app.route("/plant-alerts", methods=["GET"])
def get_plant_alerts():
    """Return the latest plant-specific environmental alerts."""
    with state_lock:
        alerts_copy = list(plant_alerts)
    return jsonify({"alerts": alerts_copy}), 200


# ─────────────────────────────────────────────────────────────────
#  ROUTES – Sensor Data
# ─────────────────────────────────────────────────────────────────
def _apply_autonomous_decisions(sensor_data: dict, plant: dict):
    """Apply auto-mode actuator decisions based on sensor readings and plant thresholds."""
    sunlight     = sensor_data.get("sunlight", 0)
    humidity     = sensor_data.get("humidity", 0)
    light_on     = plant.get("light_on_threshold",   40)
    light_off    = plant.get("light_off_threshold",  55)
    hum_min      = plant.get("humidity_min",          45)
    hum_max      = plant.get("humidity_max",          65)
    shed_close   = plant.get("shed_close_threshold", 70)
    shed_open_th = plant.get("shed_open_threshold",  50)

    # Pump: always ON
    sensor_data["pump"]        = True
    sensor_data["pump_reason"] = "Continuous — always on"

    # Light
    if sunlight < light_on:
        sensor_data["light"]        = True
        sensor_data["light_reason"] = f"Low sunlight ({sunlight:.0f}%) — lights ON"
    elif sunlight >= light_off:
        sensor_data["light"]        = False
        sensor_data["light_reason"] = f"Sufficient sunlight ({sunlight:.0f}%) — lights OFF"
    else:
        sensor_data["light_reason"] = f"Sunlight {sunlight:.0f}% — holding state"

    # Mist
    if humidity < hum_min:
        sensor_data["mist"]        = True
        sensor_data["mist_reason"] = f"Low humidity ({humidity:.0f}%) — mist ON"
    elif humidity >= hum_max:
        sensor_data["mist"]        = False
        sensor_data["mist_reason"] = f"Humidity adequate ({humidity:.0f}%) — mist OFF"
    else:
        sensor_data["mist_reason"] = f"Humidity {humidity:.0f}% — holding state"

    # Shed
    if sunlight > shed_close:
        sensor_data["shed"]        = False
        sensor_data["shed_reason"] = f"Intense sunlight ({sunlight:.0f}%) — shade CLOSED"
    elif sunlight < shed_open_th:
        sensor_data["shed"]        = True
        sensor_data["shed_reason"] = f"Low sunlight ({sunlight:.0f}%) — shade OPEN"
    else:
        sensor_data["shed_reason"] = f"Sunlight {sunlight:.0f}% — holding state"


@app.route("/sensor-data", methods=["POST"])
def receive_sensor_data():
    """Receive JSON sensor payload from ESP32."""
    global plant_alerts
    try:
        data = request.get_json(force=True, silent=True)
        if not data:
            return jsonify({"error": "Invalid JSON payload"}), 400

        ts = datetime.datetime.utcnow().isoformat() + "Z"
        with state_lock:
            # Update only fields that are present in the payload
            for key in latest_sensor_data:
                if key in data:
                    latest_sensor_data[key] = data[key]
            latest_sensor_data["timestamp"] = ts

            # ── Apply autonomous decisions if auto mode is enabled ────
            if dashboard_settings.get("auto_mode", True):
                key   = current_plant_key
                plant = plant_db.get(key, {})
                _apply_autonomous_decisions(latest_sensor_data, plant)

            # ── Append to rolling history ──
            sensor_history.append({
                "t":                ts,
                "air_temperature":  latest_sensor_data["air_temperature"],
                "humidity":         latest_sensor_data["humidity"],
                "water_temperature":latest_sensor_data["water_temperature"],
                "ph":               latest_sensor_data["ph"],
                "tds":              latest_sensor_data["tds"],
                "water_level":      latest_sensor_data["water_level"],
                "sunlight":         latest_sensor_data["sunlight"],
                "pump":             int(latest_sensor_data["pump"]),
                "light":            int(latest_sensor_data["light"]),
                "mist":             int(latest_sensor_data["mist"]),
                "shed":             int(latest_sensor_data["shed"]),
            })

            # ── Check plant-specific alerts ──────────────────────
            key   = current_plant_key
            plant = plant_db.get(key, {})
            name  = plant.get("display_name", key.title())
            new_alerts = check_plant_alerts(latest_sensor_data, plant, name)
            if new_alerts:
                plant_alerts = (new_alerts + plant_alerts)[:MAX_ALERTS]

        log.info(f"[SENSOR] Data received: temp={data.get('air_temperature')}°C "
                 f"pH={data.get('ph')} TDS={data.get('tds')}ppm")

        # Persist to Supabase asynchronously (non-blocking)
        import threading
        threading.Thread(
            target=_pg_insert_reading,
            args=(dict(latest_sensor_data), ts),
            daemon=True
        ).start()

        return jsonify({"status": "ok"}), 200

    except Exception as e:
        log.error(f"[SENSOR] POST error: {e}", exc_info=True)
        return jsonify({"error": str(e)}), 500


@app.route("/sensor-data", methods=["GET"])
def get_sensor_data():
    """Return latest sensor data to dashboard."""
    with state_lock:
        return jsonify(latest_sensor_data), 200


@app.route("/history", methods=["GET"])
def get_history():
    """
    Return rolling time-series history for all sensors.
    Query param: ?n=60  (default 60, max HISTORY_MAX)
    Response: { labels:[...], datasets:{air_temperature:[...], ...} }
    """
    try:
        n = min(int(request.args.get("n", 60)), HISTORY_MAX)
    except (ValueError, TypeError):
        n = 60

    with state_lock:
        entries = list(sensor_history)[-n:]

    if not entries:
        return jsonify({"labels": [], "datasets": {}}), 200

    keys = ["air_temperature", "humidity", "water_temperature",
            "ph", "tds", "water_level", "sunlight",
            "pump", "light", "mist", "shed"]

    def fmt_label(iso: str) -> str:
        try:
            dt = datetime.datetime.fromisoformat(iso.replace("Z", "+00:00"))
            import datetime as _dt
            local = dt + _dt.timedelta(hours=5, minutes=30)   # IST offset
            return local.strftime("%H:%M:%S")
        except Exception:
            return iso

    labels   = [fmt_label(e["t"]) for e in entries]
    datasets = {k: [e.get(k, 0) for e in entries] for k in keys}

    return jsonify({"labels": labels, "datasets": datasets}), 200


@app.route("/insights", methods=["GET"])
def get_insights():
    """
    Analyse sensor history to reveal correlations and daily patterns.
    Returns:
      - condition_stats: avg sensor values split by sunny / moderate / cloudy
      - correlations:    Pearson r between sunlight and each other sensor
      - peaks:           time-of-day when each metric peaked
      - insights:        ranked list of human-readable insight strings
      - scatter:         [{x: sunlight, y: air_temp}, ...] for scatter plot
    """
    import math

    with state_lock:
        entries = list(sensor_history)
        key     = current_plant_key
        plant   = dict(plant_db.get(key, {}))

    if len(entries) < 3:
        return jsonify({"error": "Not enough data yet – wait a few seconds."}), 200

    # ── Helpers ──────────────────────────────────────────────────
    def mean(lst):
        return sum(lst) / len(lst) if lst else 0.0

    def pearson(xs, ys):
        n = len(xs)
        if n < 2:
            return 0.0
        mx, my = mean(xs), mean(ys)
        num = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
        dx  = math.sqrt(sum((x - mx) ** 2 for x in xs))
        dy  = math.sqrt(sum((y - my) ** 2 for y in ys))
        return round(num / (dx * dy), 3) if dx * dy else 0.0

    # ── Segment by sunlight level ─────────────────────────────────
    sunny    = [e for e in entries if e.get("sunlight", 0) >= 60]
    moderate = [e for e in entries if 30 <= e.get("sunlight", 0) < 60]
    cloudy   = [e for e in entries if e.get("sunlight", 0) < 30]

    def cond_avg(segs, key):
        vals = [e.get(key, 0) for e in segs]
        return round(mean(vals), 2) if vals else None

    sensor_keys = ["air_temperature", "humidity", "water_temperature",
                   "ph", "tds", "water_level"]

    condition_stats = {}
    for cond, segs in [("sunny", sunny), ("moderate", moderate), ("cloudy", cloudy)]:
        condition_stats[cond] = {
            "count":   len(segs),
            "pct":     round(len(segs) / len(entries) * 100, 1),
        }
        for k in sensor_keys:
            condition_stats[cond][k] = cond_avg(segs, k)

    # ── Pearson correlations: sunlight vs everything ──────────────
    sun_vals = [e.get("sunlight", 0) for e in entries]
    correlations = {}
    for k in sensor_keys:
        other  = [e.get(k, 0) for e in entries]
        correlations[k] = pearson(sun_vals, other)

    # ── Peak values ──────────────────────────────────────────────
    def peak_entry(key):
        if not entries:
            return None
        best = max(entries, key=lambda e: e.get(key, 0))
        return {"value": round(best.get(key, 0), 2), "time": best["t"]}

    peaks = {k: peak_entry(k) for k in ["sunlight", "air_temperature",
                                          "humidity", "tds", "water_level"]}

    # ── Auto-generate insights (plant-aware) ──────────────────────
    insights = []
    n = len(entries)

    sun_avg  = mean(sun_vals)
    temp_all = [e.get("air_temperature", 0) for e in entries]
    hum_all  = [e.get("humidity", 0)        for e in entries]
    tds_all  = [e.get("tds", 0)             for e in entries]
    wl_all   = [e.get("water_level", 0)     for e in entries]

    plant_name   = plant.get("display_name", key.title())
    temp_min     = plant.get("temperature_min", 18)
    temp_max     = plant.get("temperature_max", 26)
    hum_min      = plant.get("humidity_min", 45)
    hum_max      = plant.get("humidity_max", 75)
    tds_min      = plant.get("tds_min", 500)
    tds_max      = plant.get("tds_max", 1000)

    # Sunlight condition summary
    pct_sunny = condition_stats["sunny"]["pct"]
    if pct_sunny > 60:
        insights.append({"level": "warning", "icon": "☀️",
            "text": f"It's been mostly sunny ({pct_sunny:.0f}% of readings) — the shade system is actively protecting your plants."})
    elif pct_sunny > 30:
        insights.append({"level": "info", "icon": "🌤️",
            "text": f"Mixed sunlight today ({pct_sunny:.0f}% sunny periods). Grow light is auto-supplementing during cloudy intervals."})
    else:
        insights.append({"level": "ok", "icon": "☁️",
            "text": f"Low sunlight day ({pct_sunny:.0f}% sunny). Grow light has been ON most of the session."})

    # Temperature vs plant range
    temp_now = temp_all[-1] if temp_all else 0
    if temp_now > temp_max:
        insights.append({"level": "warning", "icon": "🌡️",
            "text": f"Air temperature ({temp_now:.1f}°C) exceeds {plant_name}'s max of {temp_max}°C. Consider cooling."})
    elif temp_now < temp_min:
        insights.append({"level": "warning", "icon": "🌡️",
            "text": f"Air temperature ({temp_now:.1f}°C) is below {plant_name}'s min of {temp_min}°C. Warming recommended."})
    else:
        insights.append({"level": "ok", "icon": "✅",
            "text": f"Temperature ({temp_now:.1f}°C) is within ideal range for {plant_name} ({temp_min}–{temp_max}°C)."})

    # Humidity
    hum_now = hum_all[-1] if hum_all else 0
    if hum_now < hum_min:
        insights.append({"level": "warning", "icon": "💧",
            "text": f"Humidity ({hum_now:.0f}%) below {plant_name}'s min of {hum_min}%. Mist maker may need adjustment."})
    elif hum_now > hum_max:
        insights.append({"level": "info", "icon": "💧",
            "text": f"Humidity ({hum_now:.0f}%) above {plant_name}'s ideal max of {hum_max}%. Risk of mold if persistent."})

    # Water level warning
    wl_now = wl_all[-1] if wl_all else 100
    if wl_now < 25:
        insights.append({"level": "critical", "icon": "🚨",
            "text": f"⚠️ Water level is very low ({wl_now:.0f}%)! Refill the reservoir soon."})
    elif wl_now < 50:
        insights.append({"level": "warning", "icon": "🪣",
            "text": f"Water level at {wl_now:.0f}%. Consider topping up soon."})

    # TDS / nutrients vs plant range
    tds_now = tds_all[-1] if tds_all else 0
    if tds_now < tds_min:
        insights.append({"level": "warning", "icon": "🧪",
            "text": f"TDS ({tds_now:.0f} ppm) is below {plant_name}'s minimum of {tds_min} ppm. Add nutrients."})
    elif tds_now > tds_max:
        insights.append({"level": "warning", "icon": "🧪",
            "text": f"TDS ({tds_now:.0f} ppm) exceeds {plant_name}'s max of {tds_max} ppm. Dilute to avoid burn."})
    else:
        insights.append({"level": "ok", "icon": "🧪",
            "text": f"TDS ({tds_now:.0f} ppm) is in the ideal range for {plant_name} ({tds_min}–{tds_max} ppm)."})

    # ── Scatter data (sunlight vs temp, sampled every 3rd point) ─
    scatter = [
        {"x": e.get("sunlight", 0), "y": e.get("air_temperature", 0)}
        for i, e in enumerate(entries) if i % 3 == 0
    ]

    return jsonify({
        "condition_stats":  condition_stats,
        "correlations":     correlations,
        "peaks":            peaks,
        "insights":         insights,
        "scatter":          scatter,
        "data_points":      n,
        "current_plant":    plant_name,
    }), 200


# ─────────────────────────────────────────────────────────────────
#  ROUTES – Image Upload & AI Detection
# ─────────────────────────────────────────────────────────────────
@app.route("/upload-image", methods=["POST"])
def upload_image():
    """
    Receive raw JPEG image from ESP32-CAM (Content-Type: image/jpeg)
    or a multipart file upload. Run Gemini detection and store result.
    """
    try:
        # Determine source of image bytes
        content_type = request.content_type or ""

        if "image/jpeg" in content_type or "image/" in content_type:
            # Raw binary from ESP32-CAM
            image_bytes = request.data
        elif "multipart/form-data" in content_type:
            # Multipart upload (e.g., from browser/test)
            if "image" not in request.files:
                return jsonify({"error": "No 'image' field in multipart form"}), 400
            image_bytes = request.files["image"].read()
        else:
            # Fallback: try to decode base64 JSON
            data = request.get_json(force=True, silent=True)
            if data and "image_base64" in data:
                image_bytes = base64.b64decode(data["image_base64"])
            else:
                return jsonify({"error": "Unsupported content type or missing image"}), 400

        if not image_bytes:
            return jsonify({"error": "Empty image data"}), 400

        if len(image_bytes) > MAX_IMAGE_SIZE:
            return jsonify({"error": f"Image too large (max {MAX_IMAGE_SIZE // 1024} KB)"}), 413

        log.info(f"[IMAGE] Received {len(image_bytes):,} bytes from {request.remote_addr}")

        # Save image to disk (latest.jpg)
        latest_path = UPLOAD_DIR / "latest.jpg"
        latest_path.write_bytes(image_bytes)
        log.info(f"[IMAGE] Saved to {latest_path}")

        # ── Run Gemini analysis ───────────────────────────────
        analysis = analyze_image_with_gemini(image_bytes)

        with state_lock:
            latest_ai_result.update(analysis)

        log.info(f"[AI] Result – Disease: {analysis['disease_name']} | "
                 f"Confidence: {analysis['confidence']}% | "
                 f"Status: {analysis.get('status')}")

        return jsonify({
            "status":   "analyzed",
            "result":   analysis,
        }), 200

    except Exception as e:
        log.error(f"[IMAGE] Upload error: {e}", exc_info=True)
        return jsonify({"error": str(e)}), 500


@app.route("/ai-result", methods=["GET"])
def get_ai_result():
    """Return latest AI disease detection result."""
    with state_lock:
        return jsonify(latest_ai_result), 200


# ─────────────────────────────────────────────────────────────────
#  ROUTES – Actuator Control
# ─────────────────────────────────────────────────────────────────
@app.route("/control", methods=["GET"])
def get_control():
    """
    ESP32 polls this to receive pending commands.
    Dashboard reads should not consume commands, or they can clear the
    queue before the ESP32 sees it.
    """
    source = request.args.get("source", "").lower()
    consume = source == "esp32"

    with state_lock:
        # Build response with only set commands
        response = {k: v for k, v in pending_commands.items() if v is not None}
        # Only the ESP32 should consume the queue.
        if consume:
            for k in response:
                pending_commands[k] = None

    log.info(f"[CONTROL] GET from {source or 'dashboard'} – delivering {response}{' and clearing' if consume else ' without clearing'}.")
    return jsonify(response), 200


@app.route("/autonomous-status", methods=["GET"])
def get_autonomous_status():
    """Return current actuator states with autonomous reasoning for dashboard."""
    with state_lock:
        return jsonify({
            "pump":  {
                "state":  latest_sensor_data.get("pump",  False),
                "reason": latest_sensor_data.get("pump_reason",  "—"),
                "auto":   True,
            },
            "light": {
                "state":  latest_sensor_data.get("light", False),
                "reason": latest_sensor_data.get("light_reason", "—"),
                "auto":   True,
            },
            "mist":  {
                "state":  latest_sensor_data.get("mist",  False),
                "reason": latest_sensor_data.get("mist_reason",  "—"),
                "auto":   True,
            },
            "shed":  {
                "state":  latest_sensor_data.get("shed",  False),
                "reason": latest_sensor_data.get("shed_reason",  "—"),
                "auto":   True,
            },
            "thresholds": {
                "shed_close":    latest_sensor_data.get("shed_close_threshold", 70),
                "light_on":      latest_sensor_data.get("light_on_threshold",  40),
                "humidity_min":  latest_sensor_data.get("humidity_min",        45),
                "humidity_max":  latest_sensor_data.get("humidity_max",        65),
            },
            "timestamp": latest_sensor_data.get("timestamp"),
            "auto_mode": dashboard_settings.get("auto_mode", True),
        }), 200


@app.route("/control", methods=["POST"])
def set_control():
    """
    Dashboard POSTs control commands here.
    Body: {"pump": true/false, "light": true/false, ...}
    """
    try:
        data = request.get_json(force=True, silent=True)
        if not data:
            return jsonify({"error": "Invalid JSON"}), 400

        valid_keys = {"pump", "light", "mist", "shed"}
        updated = {}

        with state_lock:
            for key, value in data.items():
                if key in valid_keys and isinstance(value, bool):
                    pending_commands[key] = value
                    latest_sensor_data[key] = value  # Optimistic update for dashboard
                    latest_sensor_data[f"{key}_reason"] = "Manual override active"
                    updated[key] = value
                    log.info(f"[CONTROL] {key.upper()} → {'ON' if value else 'OFF'}")

        if not updated:
            return jsonify({"error": "No valid commands found"}), 400

        return jsonify({"status": "queued", "commands": updated}), 200

    except Exception as e:
        log.error(f"[CONTROL] POST error: {e}", exc_info=True)
        return jsonify({"error": str(e)}), 500


# ─────────────────────────────────────────────────────────────────
#  ROUTES – Dashboard Settings
# ─────────────────────────────────────────────────────────────────
@app.route("/dashboard-settings", methods=["GET"])
def get_dashboard_settings():
    """Return all persisted dashboard settings."""
    with state_lock:
        settings_copy = dict(dashboard_settings)
        settings_copy["current_plant"] = current_plant_key  # Always in sync
    return jsonify(settings_copy), 200


@app.route("/dashboard-settings", methods=["POST"])
def post_dashboard_settings():
    """
    Update one or more dashboard settings.
    Body: { "active_chart_tab": "water", "refresh_interval_ms": 3000, "notes": "...", "auto_mode": true }
    Note: 'current_plant' is managed via /set-plant — it is ignored here.
    """
    global dashboard_settings
    allowed_keys = {"active_chart_tab", "refresh_interval_ms", "notes", "auto_mode"}
    try:
        data = request.get_json(force=True, silent=True)
        if not data:
            return jsonify({"error": "Invalid JSON"}), 400

        updated = {}
        with state_lock:
            for k, v in data.items():
                if k in allowed_keys:
                    dashboard_settings[k] = v
                    updated[k] = v
            save_settings(dashboard_settings)

        log.info(f"[SETTINGS] Updated: {updated}")
        return jsonify({"status": "ok", "updated": updated}), 200

    except Exception as e:
        log.error(f"[SETTINGS] POST error: {e}", exc_info=True)
        return jsonify({"error": str(e)}), 500


# ─────────────────────────────────────────────────────────────────
#  ROUTES – Tank Configuration
# ─────────────────────────────────────────────────────────────────
@app.route("/tank-config", methods=["GET"])
def get_tank_config():
    """Get current tank dimensions and sensor offset."""
    try:
        conn = _get_conn()
        cur = conn.cursor()
        cur.execute("SELECT * FROM tank_config WHERE id=1")
        cfg = cur.fetchone()
        conn.close()
        if not cfg:
            return jsonify({"tank_height_cm": 30, "tank_width_cm": 30,
                            "tank_length_cm": 30, "sensor_offset_cm": 0}), 200
        return jsonify({
            "tank_height_cm":   cfg["tank_height_cm"],
            "tank_width_cm":    cfg["tank_width_cm"],
            "tank_length_cm":   cfg["tank_length_cm"],
            "sensor_offset_cm": cfg["sensor_offset_cm"],
            "updated_at":       cfg["updated_at"],
        }), 200
    except Exception as e:
        log.error(f"[TANK] get error: {e}")
        return jsonify({"error": str(e)}), 500


@app.route("/tank-config", methods=["POST"])
def set_tank_config():
    """Save tank dimensions and sensor offset."""
    try:
        data = request.get_json(force=True, silent=True)
        if not data:
            return jsonify({"error": "Invalid JSON"}), 400
        height = float(data.get("tank_height_cm", 30))
        width  = float(data.get("tank_width_cm",  30))
        length = float(data.get("tank_length_cm", 30))
        offset = float(data.get("sensor_offset_cm", 0))
        if height <= 0 or width <= 0 or length <= 0:
            return jsonify({"error": "Dimensions must be positive"}), 400
        now = datetime.datetime.utcnow().isoformat() + "Z"
        conn = _get_conn()
        cur = conn.cursor()
        cur.execute(
            "UPDATE tank_config SET tank_height_cm=%s, tank_width_cm=%s, tank_length_cm=%s, sensor_offset_cm=%s, updated_at=%s WHERE id=1",
            (height, width, length, offset, now)
        )
        conn.commit()
        conn.close()
        log.info(f"[TANK] Config saved: {height}×{width}×{length}cm, offset={offset}cm")
        return jsonify({"status": "ok", "tank_height_cm": height, "tank_width_cm": width,
                        "tank_length_cm": length, "sensor_offset_cm": offset}), 200
    except Exception as e:
        log.error(f"[TANK] set error: {e}")
        return jsonify({"error": str(e)}), 500


@app.route("/water-level-liters", methods=["GET"])
def get_water_level_liters():
    """Return current water volume in liters based on tank config."""
    try:
        conn = _get_conn()
        cur = conn.cursor()
        cur.execute("SELECT water_level FROM sensor_readings ORDER BY timestamp DESC LIMIT 1")
        latest = cur.fetchone()
        cur.execute("SELECT * FROM tank_config WHERE id=1")
        cfg = cur.fetchone()
        conn.close()

        water_level_raw = latest["water_level"] if latest else 0
        height = cfg["tank_height_cm"]   if cfg else 30
        width  = cfg["tank_width_cm"]    if cfg else 30
        length = cfg["tank_length_cm"]   if cfg else 30
        offset = cfg["sensor_offset_cm"] if cfg else 0

        water_level_cm       = ((water_level_raw / 100) * height) - offset
        water_level_cm       = max(0, min(water_level_cm, height))
        tank_capacity_liters = (width * length * height) / 1000
        water_volume_liters  = (width * length * water_level_cm) / 1000

        return jsonify({
            "water_level_cm":       round(water_level_cm, 2),
            "water_volume_liters":  round(water_volume_liters, 2),
            "tank_capacity_liters": round(tank_capacity_liters, 2),
            "water_level_percent":  round((water_volume_liters / tank_capacity_liters) * 100, 1) if tank_capacity_liters > 0 else 0,
        }), 200
    except Exception as e:
        log.error(f"[TANK] water-level-liters error: {e}")
        return jsonify({"error": str(e)}), 500


# ─────────────────────────────────────────────────────────────────
#  ROUTES – Frontend & Utilities
# ─────────────────────────────────────────────────────────────────
@app.route("/")
def serve_dashboard():
    """Serve the dashboard index.html."""
    return send_from_directory(str(FRONTEND_DIR), "index.html")


@app.route("/latest-image")
def serve_latest_image():
    """Serve the latest captured plant image."""
    img_path = UPLOAD_DIR / "latest.jpg"
    if img_path.exists():
        return send_from_directory(str(UPLOAD_DIR), "latest.jpg",
                                   mimetype="image/jpeg",
                                   max_age=0)
    return jsonify({"error": "No image available yet"}), 404


@app.route("/health")
def health_check():
    """Simple liveness check."""
    with state_lock:
        pkey = current_plant_key
    return jsonify({
        "status":        "ok",
        "uptime":        str(datetime.datetime.utcnow()),
        "gemini":        bool(GEMINI_API_KEY and GEMINI_API_KEY != "YOUR_GEMINI_API_KEY_HERE"),
        "current_plant": pkey,
        "total_plants":  len(plant_db),
        "auto_mode":     dashboard_settings.get("auto_mode", True),
    }), 200


# ─────────────────────────────────────────────────────────────────
#  ENTRY POINT
# ─────────────────────────────────────────────────────────────────
if __name__ == "__main__":
    log.info("=" * 60)
    log.info(" Smart Hydroponic Farming System – Flask Backend")
    log.info("=" * 60)
    log.info(f" Gemini API key configured: {bool(GEMINI_API_KEY and GEMINI_API_KEY != 'YOUR_GEMINI_API_KEY_HERE')}")
    log.info(f" Upload directory: {UPLOAD_DIR.resolve()}")
    log.info(f" Plant profiles loaded: {len(plant_db)} ({', '.join(plant_db.keys())})")
    log.info(f" Active plant: {current_plant_key}")
    log.info(" Starting server on 0.0.0.0:5000 …")
    log.info("=" * 60)

    app.run(
        host="0.0.0.0",
        port=5000,
        debug=False,      # Set True for development, False for production
        threaded=True,    # Handle concurrent requests from ESP32 + dashboard
    )
