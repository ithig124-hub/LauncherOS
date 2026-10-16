// Fusion Watch standalone environment firmware
// Target: Waveshare ESP32-S3-Touch-AMOLED-2.06, Arduino, LVGL 8.4.x
// Build each environment as its own .ino by changing FUSION_ENV below.
// 1=TravelOS, 2=ExplorerOS, 3=ScienceOS, 4=AstroOS
#define FUSION_ENV 2

#include <Arduino.h>
#include <FS.h>
#include <SD_MMC.h>
#include <Wire.h>
#include <WiFi.h>
#include <Preferences.h>
#include <time.h>
#include <sys/time.h>
#include <memory>
#include <math.h>
#include <float.h>
#include <ctype.h>
#include "Arduino_GFX_Library.h"
#include "Arduino_DriveBus_Library.h"
#include "lv_conf.h"
#include "lvgl.h"
#define XPOWERS_CHIP_AXP2101
#include "XPowersLib.h"
#include "SensorQMI8658.hpp"
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <esp_sleep.h>
#include <esp_bt.h>

#if LVGL_VERSION_MAJOR != 8 || LVGL_VERSION_MINOR != 4
#error "Fusion Environment OS requires LVGL 8.4.x"
#endif
#if !LV_USE_KEYBOARD || !LV_USE_TEXTAREA
#error "Enable LV_USE_KEYBOARD and LV_USE_TEXTAREA in lv_conf.h"
#endif

#define LCD_SDIO0 4
#define LCD_SDIO1 5
#define LCD_SDIO2 6
#define LCD_SDIO3 7
#define LCD_SCLK 11
#define LCD_CS 12
#define LCD_RESET 8
#define LCD_W 410
#define LCD_H 502
#define IIC_SDA 15
#define IIC_SCL 14
#define TP_INT 38
#define TP_RESET 9
#define BOOT_BTN 0
static const int SDMMC_CLK=2, SDMMC_CMD=1, SDMMC_DATA=3;

// This watch build has no GPS/GNSS hardware. The legacy parser remains
// compile-time disabled so the sketch never presents satellite positioning.
#ifndef EXPLORER_GPS_RX_PIN
#define EXPLORER_GPS_RX_PIN -1
#endif
#ifndef EXPLORER_GPS_TX_PIN
#define EXPLORER_GPS_TX_PIN -1
#endif
#ifndef EXPLORER_GPS_BAUD
#define EXPLORER_GPS_BAUD 9600
#endif

// QMC5883L-compatible magnetometer is auto-detected at 0x0D.
#define EXPLORER_MAG_ADDRESS 0x0D

#define SCREEN_OFF_TIMEOUT_MS 15000UL
#define WAKE_GRACE_MS 2000UL
#define CLICK_AFTER_SWIPE_MS 350UL
#define SWIPE_THRESHOLD_PX 25
#define SWIPE_MAX_DURATION_MS 800UL
#define SCREEN_SWITCH_ANIM_MS 120
#define DEFAULT_BRIGHTNESS 180
#define RETURN_HOLD_MS 1500UL
#define COL_BG 0x000000
#define COL_CARD 0x0D1117
#define COL_BORDER 0x26303A
#define COL_TEXT 0xF4F7FA
#define COL_MUTED 0x85919D
#define COL_PANEL 0x101A18
#define COL_GLOW 0x1D5A3B
#define COL_WARN 0xFFB84D

struct Feature { const char *name; const char *summary; };

#if FUSION_ENV == 1
static const char *OS_NAME="TravelOS";
static const char *OS_MARK="TRAVEL//OS";
static const char *OS_FOLDER="/WATCH/TravelOS";
static const uint32_t ACCENT=0x22D3EE;
static const char *SUBDIRS[]={"TRIPS","MAPS","PLACES","PHRASES","CURRENCY","ITINERARIES","DOCUMENTS","CHECKLISTS","EXPORTS","LOGS","CACHE","SETTINGS"};
static const Feature FEATURES[]={
 {"Trip","Offline trip, city, stay and meeting-point workspace"},
 {"Itinerary","Upcoming, active, completed, missed and cancelled events"},
 {"Map","Imported offline maps; GPS status is always identified"},
 {"Phrases","Offline phrase categories, favourites and pronunciation guides"},
 {"Places","Coordinates, notes, categories and meeting points"},
 {"Currency","Manual stored rates with a visible last-updated warning"},
 {"World clock","Home, destination and UTC time comparison"},
 {"Packing","Reusable travel and school-trip checklists"},
 {"Documents","Local reference documents; do not store secrets unprotected"},
 {"Power","Offline-first mode with radios disabled by default"},
 {"Notes","Timestamped travel notes stored under WATCH/TravelOS"},
 {"Export","Versioned trip exports for compatible Fusion environments"}
};
#elif FUSION_ENV == 2
static const char *OS_NAME="ExplorerOS";
static const char *OS_MARK="EXPLORER//OS";
static const char *OS_FOLDER="/WATCH/ExplorerOS";
static const uint32_t ACCENT=0x65D46E;
static const char *SUBDIRS[]={"MAPS","ROUTES","WAYPOINTS","TRACKS","FIELD_NOTES","SURVEYS","GUIDES","SENSOR_LOGS","EXPORTS","LOGS","CACHE","SETTINGS"};
static const Feature FEATURES[]={
 {"Adventure guide","Offline guidance for planning, shelter, water and emergencies"},
 {"Trail map","Relative IMU trail, imported reference points and markers"},
 {"Motion compass","Gyroscope-relative heading with user-set north"},
 {"Trail session","Step distance, pace, time, pause, save and notes"},
 {"Trail markers","Relative destination, meeting, landmark and hazard records"},
 {"Pace + bearing","Estimated movement and bearing with drift warnings"},
 {"Field notes","Trail-relative notes, readings, times and tags"},
 {"Survey","Relative distance, bearing, polygon area and stride setup"},
 {"Sensors","Live QMI8658 accelerometer and hardware health"},
 {"Power modes","Full, trail saver, map, sensor-sleep and low-battery modes"},
 {"Track log","Versioned CSV sensor and route samples"},
 {"Export","Validated routes and waypoints for Fusion sharing"},
 {"Backtrack","Return-to-camp bearing, distance and drift-aware guidance"},
 {"Adventure timers","Hydration reminders, stopwatch and distance goal"}
};
#elif FUSION_ENV == 3
static const char *OS_NAME="ScienceOS";
static const char *OS_MARK="SCIENCE//OS";
static const char *OS_FOLDER="/WATCH/ScienceOS";
static const uint32_t ACCENT=0xA78BFA;
static const char *SUBDIRS[]={"EXPERIMENTS","DATASETS","GRAPHS","MEASUREMENTS","FORMULAS","REFERENCE","NOTES","EXPORTS","LOGS","CACHE","SETTINGS"};
static const Feature FEATURES[]={
 {"Calculator","Scientific operations, constants and degree/radian modes"},
 {"Units","Length, mass, time, motion, energy, electrical and SI units"},
 {"Physics","Motion, force, energy, waves, gravity and projectiles"},
 {"Electronics","Ohm law, dividers, LED resistors and component reference"},
 {"Chemistry","Moles, molarity, dilution, elements and significant figures"},
 {"Biology","Offline educational reference; never a diagnostic system"},
 {"Experiment","Hypothesis, variables, procedure, observations and results"},
 {"Data logger","IMU, battery and optional external sensor datasets"},
 {"Graphs","Line, scatter, statistics and sample inspection workspace"},
 {"Signals","Peak, RMS, period, noise estimate and smoothing tools"},
 {"Reference","Constants, equations, definitions, units and examples"},
 {"Export","Versioned datasets for analysis in other environments"}
};
#elif FUSION_ENV == 4
static const char *OS_NAME="AstroOS";
static const char *OS_MARK="ASTRO//OS";
static const char *OS_FOLDER="/WATCH/AstroOS";
static const uint32_t ACCENT=0xF5B942;
static const char *SUBDIRS[]={"STARS","CONSTELLATIONS","PLANETS","MOON","SATELLITES","EVENTS","OBSERVATIONS","CATALOGUES","LOCATIONS","EXPORTS","LOGS","CACHE","SETTINGS"};
static const Feature FEATURES[]={
 {"Sky compass","Azimuth and tilt guidance; compass hardware is optional"},
 {"Stars","Imported offline star catalogue, magnitude and favourites"},
 {"Constellations","Small-screen diagrams and direction guidance"},
 {"Planets","Stored facts and calculated approximate visibility"},
 {"Moon","Calculated phase and illumination from the current clock"},
 {"Sun","Sunrise and twilight require a saved location"},
 {"Events","Offline phases, seasons, showers and imported eclipse data"},
 {"Satellites","Passes require current imported orbital elements"},
 {"Orbit tools","Period, velocity, gravity and escape-velocity workspace"},
 {"Observe","Timestamped object, sky, equipment and rating log"},
 {"Locations","Saved observing sites for calculations"},
 {"Export","Versioned observations and locations for Fusion sharing"}
};
#else
#error "FUSION_ENV must be 1, 2, 3 or 4"
#endif

static const size_t FEATURE_COUNT=sizeof(FEATURES)/sizeof(FEATURES[0]);
static const size_t SUBDIR_COUNT=sizeof(SUBDIRS)/sizeof(SUBDIRS[0]);

Arduino_DataBus *g_bus=new Arduino_ESP32QSPI(LCD_CS,LCD_SCLK,LCD_SDIO0,LCD_SDIO1,LCD_SDIO2,LCD_SDIO3);
Arduino_GFX *gfx=new Arduino_CO5300(g_bus,LCD_RESET,0,LCD_W,LCD_H,22,0,0,0);
std::shared_ptr<Arduino_IIC_DriveBus> g_iic=std::make_shared<Arduino_HWIIC>(IIC_SDA,IIC_SCL,&Wire);
void Arduino_IIC_Touch_Interrupt(void);
std::unique_ptr<Arduino_IIC> FT3168(new Arduino_FT3x68(g_iic,FT3168_DEVICE_ADDRESS,DRIVEBUS_DEFAULT_VALUE,TP_INT,Arduino_IIC_Touch_Interrupt));
void Arduino_IIC_Touch_Interrupt(void){ FT3168->IIC_Interrupt_Flag=true; }
XPowersPMU g_pmu;
SensorQMI8658 g_imu;

static bool g_has_pmu=false, g_has_imu=false, g_sd_ok=false;
static lv_disp_draw_buf_t g_draw_buf;
static lv_color_t *g_lvbuf=nullptr;
static lv_disp_drv_t g_disp_drv;
static lv_indev_drv_t g_indev_drv;
static lv_obj_t *g_screens[3]={nullptr,nullptr,nullptr};
static lv_obj_t *g_clock=nullptr, *g_battery=nullptr, *g_sensor=nullptr, *g_record_label=nullptr;
static int g_screen=0;
static volatile int g_pending_swipe=0;
static int32_t g_start_x=0,g_start_y=0,g_current_x=0,g_current_y=0;
static uint32_t g_start_ms=0,g_last_touch_ms=0,g_last_wake_ms=0,g_last_swipe_ms=0;
static bool g_touch_active=false,g_screen_off=false,g_returning=false,g_recording=false;
static uint8_t g_brightness=0;
static File g_log_file;
static float g_ax=0,g_ay=0,g_az=0;
static uint32_t g_last_clock_save_ms=0;

// ============================================================================
// Explorer engine: measured navigation, map, route, waypoint and survey data
// ============================================================================
#define MAX_WAYPOINTS 40
#define MAX_MAP_POINTS 80
#define TRACK_RAM_POINTS 256
#define EARTH_RADIUS_M 6371000.0

struct GpsState {
  bool port=false, valid=false;
  double lat=0, lon=0, altitude=0;
  float speed_kmh=0, course_deg=0, hdop=99, vdop=99;
  uint8_t satellites=0, fix_type=0;
  int8_t module_battery_pct=-1;
  uint32_t last_sentence_ms=0, last_fix_ms=0, sequence=0;
};
struct TrackPoint { double lat,lon; float altitude; uint32_t unix_time; };
struct Waypoint {
  char name[24],category[16],note[44];
  double lat,lon; float altitude,local_east,local_north; uint32_t unix_time; bool favourite,arrived,geodetic;
};
struct MapPoint { double lat,lon; char type[12],name[24]; bool local; };
struct MagState {
  bool present=false,reliable=false,calibrating=false;
  int16_t raw_x=0,raw_y=0,raw_z=0;
  float heading=0, field=0;
  int16_t min_x=32767,min_y=32767,min_z=32767,max_x=-32768,max_y=-32768,max_z=-32768;
};
enum RouteState { ROUTE_IDLE,ROUTE_ACTIVE,ROUTE_PAUSED,ROUTE_SAVED };
enum PowerMode { POWER_FULL,POWER_TRAIL_SAVER,POWER_LOW_ROUTE,POWER_COMPASS_ONLY,POWER_OFFLINE_MAP,POWER_SENSOR_SLEEP,POWER_LOW_BATTERY };

static GpsState GPS;
static MagState MAG;
static float g_declination_deg=0;
static Waypoint WAYPOINTS[MAX_WAYPOINTS]; static int g_waypoint_count=0;
static MapPoint MAP_POINTS[MAX_MAP_POINTS]; static int g_map_point_count=0;
static TrackPoint TRACK[TRACK_RAM_POINTS]; static int g_track_count=0,g_track_head=0;
static bool g_track_has_geo=false;
static RouteState g_route_state=ROUTE_IDLE;
static PowerMode g_power_mode=POWER_FULL;
static File g_route_file;
static String g_route_path="";
static uint32_t g_route_started_ms=0,g_route_paused_ms=0,g_route_pause_start=0,g_last_route_fix_seq=0;
static double g_route_distance_m=0; static float g_route_max_speed=0,g_route_gain_m=0,g_route_loss_m=0,g_last_route_alt=0;
static bool g_have_route_alt=false;
static double g_dr_lat=0,g_dr_lon=0; static float g_dr_uncertainty_m=0; static uint32_t g_dr_last_ms=0,g_step_count=0;
static float g_local_east_m=0,g_local_north_m=0,g_relative_heading=0,g_stride_m=0.75f;
static float g_track_east[TRACK_RAM_POINTS],g_track_north[TRACK_RAM_POINTS];
static uint32_t g_last_route_step=0;
static float g_imu_mean=1.0f,g_imu_prev_ac=0,g_gx=0,g_gy=0,g_gz=0;
static uint32_t g_last_step_ms=0,g_last_step_gap_ms=0,g_imu_samples=0; static float g_imu_hz=0;
static bool g_position_averaging=false; static uint16_t g_average_count=0; static double g_average_lat=0,g_average_lon=0,g_average_alt=0;
static int g_waypoint_category=0;
static const char *WAYPOINT_CATEGORIES[]={"Destination","Meeting","Landmark","Hazard","Research","Start","Finish","Custom"};
static const char *POWER_NAMES[]={"Full exploration","Trail saver","Low-power trail","Motion heading","Offline map","Sensor sleep","Low battery"};
struct GuideEntry{const char*title;const char*body;};
static const GuideEntry GUIDE[]={
 {"PLAN BEFORE LEAVING","Tell someone your route and return time. Check weather, daylight, water, clothing and battery. Turn around early when conditions worsen."},
 {"IF YOU ARE LOST","Stop, stay calm, observe and plan. Retrace only when safe. Make yourself visible and conserve energy. Emergency services instructions take priority."},
 {"WATER","Carry treated water. Clear-looking water may still be unsafe. Filter and disinfect when possible. Avoid dehydration; do not ration to a dangerous level."},
 {"SHELTER","Protect yourself from wind, rain, heat and ground cold. Use existing safe shelter first. Avoid dry creek beds, unstable trees and exposed ridges."},
 {"FIRE SAFETY","Follow local fire bans. Clear the area, keep water nearby and extinguish completely. Never light a fire when conditions or rules prohibit it."},
 {"WEATHER","Watch cloud growth, wind shifts, falling temperature and thunder. Leave exposed ground early. This watch has no barometer or live forecast."},
 {"WILDLIFE","Give animals space. Do not feed or corner them. Store food securely. Follow local advice for snakes, insects and large animals."},
 {"FIRST AID","Check danger, response, airway and breathing; call emergency services. Control severe bleeding with firm direct pressure. This is a quick reference, not training."},
 {"SIGNALLING","Three repeated signals commonly indicate distress. Use whistle, light or visible markers. SOS is three short, three long, three short."},
 {"LEAVE NO TRACE","Plan ahead, stay on durable surfaces, take waste out, leave natural objects, minimise fire effects and respect wildlife and other visitors."},
 {"BASIC KNOTS","Bowline: fixed loop. Clove hitch: temporary attachment. Reef knot: joins similar lines, not life safety. Use certified equipment for climbing or rescue."}
};
static const char*CHECK_ITEMS[]={"Route shared","Weather checked","Water packed","First aid packed","Layers packed","Watch charged","Return time set","Emergency contact"};
static int g_guide_index=0,g_check_index=0;static uint16_t g_check_bits=0;static bool g_guide_checklist=false;
static uint8_t active_brightness(){return g_power_mode==POWER_LOW_BATTERY?80:(g_power_mode==POWER_LOW_ROUTE||g_power_mode==POWER_SENSOR_SLEEP)?125:DEFAULT_BRIGHTNESS;}

static lv_obj_t *g_module_screen=nullptr,*g_module_value=nullptr,*g_module_aux=nullptr,*g_map_box=nullptr,*g_module_action=nullptr,*g_emergency=nullptr;
static bool g_signal_white=false;
static lv_obj_t *g_dash_state=nullptr,*g_dash_heading=nullptr,*g_dash_distance=nullptr,*g_dash_steps=nullptr,*g_compass_box=nullptr,*g_compass_needle=nullptr,*g_compass_readout=nullptr;
static lv_point_t g_compass_points[2];
static lv_point_t g_map_line_points[TRACK_RAM_POINTS];
static bool g_camp_set=false,g_adventure_timer_running=false;static float g_camp_east=0,g_camp_north=0,g_goal_km=5.0f;static uint16_t g_hydration_minutes=45;static uint32_t g_adventure_timer_started=0,g_adventure_elapsed_ms=0,g_hydration_due=0,g_lap_count=0;
static int g_module_id=-1;
enum EditTarget { EDIT_NONE,EDIT_ROUTE_NAME,EDIT_ROUTE_NOTE,EDIT_WAYPOINT_NAME,EDIT_WAYPOINT_NOTE,EDIT_FIELD_NOTE };
static EditTarget g_edit_target=EDIT_NONE; static lv_obj_t *g_editor=nullptr,*g_edit_area=nullptr;
static double g_map_center_lat=0,g_map_center_lon=0; static float g_map_metres_per_px=8.0f; static bool g_map_track_up=false;
static float g_map_center_east=0,g_map_center_north=0;
#if EXPLORER_GPS_RX_PIN >= 0
static char g_nmea_line[128]; static uint8_t g_nmea_len=0;
#endif

static void init_screen(lv_obj_t *s);
static void record_click(lv_event_t *e);
static void save_clock();
static bool rewrite_waypoints();
static void track_push(double lat,double lon,float altitude,uint32_t stamp,float east=0,float north=0,bool geo=true);
static void update_module();

#if EXPLORER_GPS_RX_PIN >= 0
static HardwareSerial ExplorerGPS(1);
#endif

static double radians_d(double v){ return v*M_PI/180.0; }
static double degrees_d(double v){ return v*180.0/M_PI; }
static double distance_m(double la1,double lo1,double la2,double lo2){
  double p1=radians_d(la1),p2=radians_d(la2),dp=radians_d(la2-la1),dl=radians_d(lo2-lo1);
  double a=sin(dp/2)*sin(dp/2)+cos(p1)*cos(p2)*sin(dl/2)*sin(dl/2);
  return EARTH_RADIUS_M*2*atan2(sqrt(a),sqrt(1-a));
}
static float bearing_deg(double la1,double lo1,double la2,double lo2){
  double p1=radians_d(la1),p2=radians_d(la2),dl=radians_d(lo2-lo1);
  double y=sin(dl)*cos(p2),x=cos(p1)*sin(p2)-sin(p1)*cos(p2)*cos(dl);
  float b=fmodf((float)degrees_d(atan2(y,x))+360.0f,360.0f); return b;
}
// Index-based signatures avoid Arduino's .ino prototype generator moving a
// Waypoint-dependent declaration ahead of the Waypoint definition.
static float local_distance_to(int waypoint_index){const Waypoint&w=WAYPOINTS[waypoint_index];return hypotf(w.local_east-g_local_east_m,w.local_north-g_local_north_m);}
static float local_bearing_to(int waypoint_index){const Waypoint&w=WAYPOINTS[waypoint_index];return fmodf(atan2f(w.local_east-g_local_east_m,w.local_north-g_local_north_m)*180.0f/M_PI+360.0f,360.0f);}
static void destination_point(double lat,double lon,float bearing,float metres,double &out_lat,double &out_lon){
  double d=metres/EARTH_RADIUS_M,b=radians_d(bearing),p1=radians_d(lat),l1=radians_d(lon);
  double p2=asin(sin(p1)*cos(d)+cos(p1)*sin(d)*cos(b));
  double l2=l1+atan2(sin(b)*sin(d)*cos(p1),cos(d)-sin(p1)*sin(p2));
  out_lat=degrees_d(p2); out_lon=degrees_d(l2);
}

static bool valid_number(double v){ return isfinite(v); }
static void clean_csv(char *s){ for(;*s;s++) if(*s==','||*s=='\n'||*s=='\r') *s=' '; }

#if EXPLORER_GPS_RX_PIN >= 0
static bool nmea_checksum_ok(const char *line){
  if(!line || line[0]!='$') return false;
  const char *star=strchr(line,'*'); if(!star) return true;
  uint8_t sum=0; for(const char *p=line+1;p<star;p++) sum^=(uint8_t)*p;
  return sum==(uint8_t)strtoul(star+1,nullptr,16);
}
static double nmea_coord(const char *text,char hemi){
  if(!text||!*text) return NAN;
  double raw=atof(text); int degrees=(int)(raw/100.0); double value=degrees+(raw-degrees*100.0)/60.0;
  if(hemi=='S'||hemi=='W') value=-value;
  return value;
}
static void parse_nmea(char *line){
  if(!nmea_checksum_ok(line)) return;
  char *star=strchr(line,'*'); if(star) *star=0;
  char *fields[20]={nullptr}; int count=0; fields[count++]=line;
  for(char *p=line;*p&&count<20;p++){if(*p==','){*p=0;fields[count++]=p+1;}}
  if(count<2) return;
  const char *kind=strlen(fields[0])>=6?fields[0]+3:fields[0];
  GPS.last_sentence_ms=millis();
  if(strcmp(kind,"RMC")==0 && count>=9){
    GPS.valid=fields[2][0]=='A';
    double lat=nmea_coord(fields[3],fields[4][0]),lon=nmea_coord(fields[5],fields[6][0]);
    if(GPS.valid&&valid_number(lat)&&valid_number(lon)){
      GPS.lat=lat; GPS.lon=lon; GPS.speed_kmh=atof(fields[7])*1.852f; GPS.course_deg=atof(fields[8]); GPS.last_fix_ms=millis(); GPS.sequence++;
      static uint32_t last_clock_sync=0;
      if(count>9&&strlen(fields[1])>=6&&strlen(fields[9])>=6&&(!last_clock_sync||millis()-last_clock_sync>21600000UL)){
        struct tm utc={};utc.tm_hour=(fields[1][0]-'0')*10+fields[1][1]-'0';utc.tm_min=(fields[1][2]-'0')*10+fields[1][3]-'0';utc.tm_sec=(fields[1][4]-'0')*10+fields[1][5]-'0';utc.tm_mday=(fields[9][0]-'0')*10+fields[9][1]-'0';utc.tm_mon=((fields[9][2]-'0')*10+fields[9][3]-'0')-1;int yy=(fields[9][4]-'0')*10+fields[9][5]-'0';utc.tm_year=(yy>=80?1900+yy:2000+yy)-1900;setenv("TZ","UTC0",1);tzset();time_t stamp=mktime(&utc);if(stamp>1700000000){struct timeval tv={stamp,0};settimeofday(&tv,nullptr);time_apply_tz(g_tp_gmt);save_clock();last_clock_sync=millis();}
      }
    }
  } else if(strcmp(kind,"GGA")==0 && count>=10){
    uint8_t quality=atoi(fields[6]); GPS.valid=quality>0; GPS.fix_type=quality>0?3:0; GPS.satellites=atoi(fields[7]); GPS.hdop=atof(fields[8]); GPS.altitude=atof(fields[9]);
    double lat=nmea_coord(fields[2],fields[3][0]),lon=nmea_coord(fields[4],fields[5][0]);
    if(GPS.valid&&valid_number(lat)&&valid_number(lon)){ GPS.lat=lat; GPS.lon=lon; GPS.last_fix_ms=millis(); GPS.sequence++; }
  } else if(strcmp(kind,"GSA")==0 && count>=18){ GPS.fix_type=atoi(fields[2]); GPS.hdop=atof(fields[16]); GPS.vdop=atof(fields[17]); }
}
#endif
static void gps_tick(){
#if EXPLORER_GPS_RX_PIN >= 0
  while(ExplorerGPS.available()){
    char c=(char)ExplorerGPS.read();
    if(c=='\n'||c=='\r'){
      if(g_nmea_len){ g_nmea_line[g_nmea_len]=0; parse_nmea(g_nmea_line); g_nmea_len=0; }
    } else if(g_nmea_len<sizeof(g_nmea_line)-1) g_nmea_line[g_nmea_len++]=c; else g_nmea_len=0;
  }
#endif
  if(GPS.valid&&millis()-GPS.last_fix_ms>5000) GPS.valid=false;
}

static void qmc_write(uint8_t reg,uint8_t value){ Wire.beginTransmission(EXPLORER_MAG_ADDRESS); Wire.write(reg); Wire.write(value); Wire.endTransmission(); }
static void mag_init(){
  Wire.beginTransmission(EXPLORER_MAG_ADDRESS); MAG.present=(Wire.endTransmission()==0); if(!MAG.present) return;
  qmc_write(0x0B,0x01); qmc_write(0x09,0x1D);
  Preferences p; p.begin("explorermag",true);
  MAG.min_x=p.getShort("minx",32767); MAG.max_x=p.getShort("maxx",-32768); MAG.min_y=p.getShort("miny",32767); MAG.max_y=p.getShort("maxy",-32768); MAG.min_z=p.getShort("minz",32767); MAG.max_z=p.getShort("maxz",-32768); p.end();
  p.begin("explorernav",true);g_declination_deg=p.getFloat("declination",0);p.end();
}
static void mag_save_calibration(){
  Preferences p; p.begin("explorermag",false); p.putShort("minx",MAG.min_x); p.putShort("maxx",MAG.max_x); p.putShort("miny",MAG.min_y); p.putShort("maxy",MAG.max_y); p.putShort("minz",MAG.min_z); p.putShort("maxz",MAG.max_z); p.end();
}
static void mag_tick(){
  if(!MAG.present) return;
  static uint32_t last=0; if(millis()-last<80) return; last=millis();
  Wire.beginTransmission(EXPLORER_MAG_ADDRESS); Wire.write((uint8_t)0x00); if(Wire.endTransmission(false)!=0) return;
  if(Wire.requestFrom((uint8_t)EXPLORER_MAG_ADDRESS,(uint8_t)6)!=6) return;
  MAG.raw_x=(int16_t)(Wire.read()|Wire.read()<<8); MAG.raw_y=(int16_t)(Wire.read()|Wire.read()<<8); MAG.raw_z=(int16_t)(Wire.read()|Wire.read()<<8);
  if(MAG.calibrating){ MAG.min_x=min(MAG.min_x,MAG.raw_x); MAG.max_x=max(MAG.max_x,MAG.raw_x); MAG.min_y=min(MAG.min_y,MAG.raw_y); MAG.max_y=max(MAG.max_y,MAG.raw_y); MAG.min_z=min(MAG.min_z,MAG.raw_z); MAG.max_z=max(MAG.max_z,MAG.raw_z); }
  float ox=(MAG.max_x+MAG.min_x)/2.0f,oy=(MAG.max_y+MAG.min_y)/2.0f,oz=(MAG.max_z+MAG.min_z)/2.0f;
  float mx=MAG.raw_x-ox,my=MAG.raw_y-oy,mz=MAG.raw_z-oz;
  float roll=atan2f(g_ay,g_az),pitch=atan2f(-g_ax,sqrtf(g_ay*g_ay+g_az*g_az));
  float xh=mx*cosf(pitch)+mz*sinf(pitch),yh=mx*sinf(roll)*sinf(pitch)+my*cosf(roll)-mz*sinf(roll)*cosf(pitch);
  MAG.heading=fmodf(atan2f(yh,xh)*180.0f/M_PI+360.0f,360.0f); MAG.field=sqrtf(mx*mx+my*my+mz*mz);
  MAG.reliable=(MAG.max_x-MAG.min_x>100)&&(MAG.max_y-MAG.min_y>100)&&MAG.field>10;
}

static float navigation_heading(){
  if(MAG.present&&MAG.reliable) return fmodf(MAG.heading+g_declination_deg+360.0f,360.0f);
  return g_relative_heading;
}
static void imu_tick(){
  static uint32_t last=0,rate_mark=0,rate_count=0; uint32_t now=millis();
  uint32_t interval=(g_power_mode==POWER_SENSOR_SLEEP)?250:20; if(now-last<interval) return; last=now;
  if(!g_has_imu||!g_imu.getDataReady()) return;
  g_imu.getAccelerometer(g_ax,g_ay,g_az); g_imu.getGyroscope(g_gx,g_gy,g_gz); g_imu_samples++; rate_count++;
  float mag=sqrtf(g_ax*g_ax+g_ay*g_ay+g_az*g_az); g_imu_mean=0.96f*g_imu_mean+0.04f*mag; float ac=mag-g_imu_mean;
  float dt=interval/1000.0f;g_relative_heading=fmodf(g_relative_heading+g_gz*dt+360.0f,360.0f);
  if(g_imu_prev_ac<0.12f&&ac>=0.12f&&now-g_last_step_ms>280){
    uint32_t step_gap=g_last_step_ms?now-g_last_step_ms:0;g_last_step_ms=now;g_last_step_gap_ms=step_gap;g_step_count++;
    g_local_east_m+=g_stride_m*sinf(radians_d(g_relative_heading));g_local_north_m+=g_stride_m*cosf(radians_d(g_relative_heading));
    if(g_route_state==ROUTE_ACTIVE){g_route_distance_m+=g_stride_m;float speed=step_gap?g_stride_m/(step_gap/1000.0f)*3.6f:0;g_route_max_speed=max(g_route_max_speed,speed);track_push(0,0,0,(uint32_t)time(nullptr),g_local_east_m,g_local_north_m,false);g_route_file.printf("%lu,,,,%.2f,%.1f,,IMU_ESTIMATE,%.2f,%.2f\n",(unsigned long)time(nullptr),speed,g_relative_heading,g_local_east_m,g_local_north_m);}
  }
  g_imu_prev_ac=ac;
  if(!rate_mark) rate_mark=now;
  if(now-rate_mark>=2000){ g_imu_hz=rate_count*1000.0f/(now-rate_mark); rate_count=0; rate_mark=now; }
}

static void track_push(double lat,double lon,float altitude,uint32_t stamp,float east,float north,bool geo){
  TRACK[g_track_head]={lat,lon,altitude,stamp};g_track_east[g_track_head]=east;g_track_north[g_track_head]=north;if(geo)g_track_has_geo=true;g_track_head=(g_track_head+1)%TRACK_RAM_POINTS;if(g_track_count<TRACK_RAM_POINTS)g_track_count++;
}
static int track_index_at(int chronological){int start=(g_track_head-g_track_count+TRACK_RAM_POINTS)%TRACK_RAM_POINTS;return(start+chronological)%TRACK_RAM_POINTS;}

static void route_start(){
  if(!g_sd_ok||g_route_state==ROUTE_ACTIVE) return;
  char path[128]; snprintf(path,sizeof(path),"%s/ROUTES/route_%lu_%lu.csv",OS_FOLDER,(unsigned long)time(nullptr),(unsigned long)millis());
  g_route_path=path; g_route_file=SD_MMC.open(path,FILE_WRITE); if(!g_route_file) return;
  g_route_file.println("schema=2,source=ExplorerOS");
  g_route_file.println("unix,latitude,longitude,altitude_m,speed_kmh,heading_deg,hdop,source,local_east_m,local_north_m");
  g_route_state=ROUTE_ACTIVE; g_route_started_ms=millis(); g_route_paused_ms=0; g_route_distance_m=0; g_route_max_speed=0; g_route_gain_m=0; g_route_loss_m=0; g_track_count=0; g_track_head=0;g_track_has_geo=false;g_have_route_alt=false;g_last_route_fix_seq=0;g_last_route_step=g_step_count;
}
static void route_pause_toggle(){
  if(g_route_state==ROUTE_ACTIVE){ g_route_state=ROUTE_PAUSED; g_route_pause_start=millis(); g_route_file.flush(); }
  else if(g_route_state==ROUTE_PAUSED){ g_route_paused_ms+=millis()-g_route_pause_start; g_route_state=ROUTE_ACTIVE; }
}
static void route_stop(){
  if(g_route_state!=ROUTE_ACTIVE&&g_route_state!=ROUTE_PAUSED) return;
  if(g_route_state==ROUTE_PAUSED) g_route_paused_ms+=millis()-g_route_pause_start;
  g_route_file.printf("# summary,distance_m=%.2f,max_speed_kmh=%.2f,gain_m=%.2f,loss_m=%.2f\n",g_route_distance_m,g_route_max_speed,g_route_gain_m,g_route_loss_m);
  g_route_file.flush(); g_route_file.close(); g_route_state=ROUTE_SAVED;
}
static uint32_t route_elapsed_s(){
  if(g_route_state==ROUTE_IDLE) return 0;
  uint32_t end=(g_route_state==ROUTE_PAUSED)?g_route_pause_start:millis();
  return (end-g_route_started_ms-g_route_paused_ms)/1000;
}
static void route_tick(){
  static int last_flushed=0;if(g_route_state==ROUTE_ACTIVE&&g_track_count-last_flushed>=10){g_route_file.flush();last_flushed=g_track_count;}if(g_route_state!=ROUTE_ACTIVE)last_flushed=0;
}

static bool append_waypoint(const char *name,const char *category,const char *note,double lat,double lon,float alt,bool fav,float local_east=0,float local_north=0,bool geodetic=true){
  if(!g_sd_ok||g_waypoint_count>=MAX_WAYPOINTS||!valid_number(lat)||!valid_number(lon)) return false;
  Waypoint &w=WAYPOINTS[g_waypoint_count++]; snprintf(w.name,sizeof(w.name),"%s",name); snprintf(w.category,sizeof(w.category),"%s",category); snprintf(w.note,sizeof(w.note),"%s",note); clean_csv(w.name); clean_csv(w.category); clean_csv(w.note);
  w.lat=lat; w.lon=lon; w.altitude=alt;w.local_east=local_east;w.local_north=local_north;w.geodetic=geodetic;w.unix_time=(uint32_t)time(nullptr);w.favourite=fav;w.arrived=false;
  String p=String(OS_FOLDER)+"/WAYPOINTS/waypoints.csv"; bool new_file=!SD_MMC.exists(p.c_str()); File f=SD_MMC.open(p.c_str(),FILE_APPEND);
  if(!f){g_waypoint_count--;return false;}if(new_file)f.println("name,category,note,latitude,longitude,altitude_m,unix,favourite,arrived,local_east_m,local_north_m,geodetic");
  f.printf("%s,%s,%s,%.7f,%.7f,%.2f,%lu,%d,%d,%.2f,%.2f,%d\n",w.name,w.category,w.note,w.lat,w.lon,w.altitude,(unsigned long)w.unix_time,w.favourite?1:0,w.arrived?1:0,w.local_east,w.local_north,w.geodetic?1:0);f.flush();f.close();return true;
}
static bool save_current_waypoint(const char *note="watch waypoint"){
  char name[24];snprintf(name,sizeof(name),"MARK-%03d",g_waypoint_count+1);
  return append_waypoint(name,WAYPOINT_CATEGORIES[g_waypoint_category],note,0,0,0,false,g_local_east_m,g_local_north_m,false);
}
static void load_waypoints(){
  g_waypoint_count=0; if(!g_sd_ok) return; String p=String(OS_FOLDER)+"/WAYPOINTS/waypoints.csv"; File f=SD_MMC.open(p.c_str(),FILE_READ); if(!f) return;
  char line[220]; f.readBytesUntil('\n',line,sizeof(line)-1);
  while(f.available()&&g_waypoint_count<MAX_WAYPOINTS){size_t n=f.readBytesUntil('\n',line,sizeof(line)-1);line[n]=0;Waypoint w={};int fav=0,arrived=0,geo=0;unsigned long stamp=0;
    int got=sscanf(line,"%23[^,],%15[^,],%43[^,],%lf,%lf,%f,%lu,%d,%d,%f,%f,%d",w.name,w.category,w.note,&w.lat,&w.lon,&w.altitude,&stamp,&fav,&arrived,&w.local_east,&w.local_north,&geo);
    if(got>=7&&fabs(w.lat)<=90&&fabs(w.lon)<=180){w.unix_time=(uint32_t)stamp;w.favourite=fav!=0;w.arrived=arrived!=0;w.geodetic=got>=12?geo!=0:true;WAYPOINTS[g_waypoint_count++]=w;}
  } f.close();
}
static bool rewrite_waypoints(){
  if(!g_sd_ok)return false;
  String final_path=String(OS_FOLDER)+"/WAYPOINTS/waypoints.csv",tmp=final_path+".tmp";SD_MMC.remove(tmp.c_str());File f=SD_MMC.open(tmp.c_str(),FILE_WRITE);if(!f)return false;
  f.println("name,category,note,latitude,longitude,altitude_m,unix,favourite,arrived,local_east_m,local_north_m,geodetic");
  for(int i=0;i<g_waypoint_count;i++){Waypoint&w=WAYPOINTS[i];clean_csv(w.name);clean_csv(w.category);clean_csv(w.note);f.printf("%s,%s,%s,%.7f,%.7f,%.2f,%lu,%d,%d,%.2f,%.2f,%d\n",w.name,w.category,w.note,w.lat,w.lon,w.altitude,(unsigned long)w.unix_time,w.favourite?1:0,w.arrived?1:0,w.local_east,w.local_north,w.geodetic?1:0);}f.flush();f.close();SD_MMC.remove(final_path.c_str());return SD_MMC.rename(tmp.c_str(),final_path.c_str());
}
static void load_map_points(){
  g_map_point_count=0;if(!g_sd_ok)return;char line[160];String p=String(OS_FOLDER)+"/MAPS/map.csv";File f=SD_MMC.open(p.c_str(),FILE_READ);
  if(f){f.readBytesUntil('\n',line,sizeof(line)-1);while(f.available()&&g_map_point_count<MAX_MAP_POINTS){size_t n=f.readBytesUntil('\n',line,sizeof(line)-1);line[n]=0;MapPoint m={};if(sscanf(line,"%lf,%lf,%11[^,],%23[^\r\n]",&m.lat,&m.lon,m.type,m.name)==4&&fabs(m.lat)<=90&&fabs(m.lon)<=180){m.local=false;MAP_POINTS[g_map_point_count++]=m;}}f.close();}
  p=String(OS_FOLDER)+"/MAPS/relative_map.csv";f=SD_MMC.open(p.c_str(),FILE_READ);
  if(f){f.readBytesUntil('\n',line,sizeof(line)-1);while(f.available()&&g_map_point_count<MAX_MAP_POINTS){size_t n=f.readBytesUntil('\n',line,sizeof(line)-1);line[n]=0;MapPoint m={};if(sscanf(line,"%lf,%lf,%11[^,],%23[^\r\n]",&m.lat,&m.lon,m.type,m.name)==4){m.local=true;MAP_POINTS[g_map_point_count++]=m;}}f.close();}
}

static void add_field_note(const char *tag,const char *text){
  if(!g_sd_ok) return;
  String p=String(OS_FOLDER)+"/FIELD_NOTES/field_notes.csv"; bool fresh=!SD_MMC.exists(p.c_str()); File f=SD_MMC.open(p.c_str(),FILE_APPEND); if(!f)return;
  if(fresh) f.println("unix,latitude,longitude,local_east_m,local_north_m,source,tag,note");
  f.printf("%lu,,,%.2f,%.2f,IMU_RELATIVE,%s,%s\n",(unsigned long)time(nullptr),g_local_east_m,g_local_north_m,tag,text); f.flush(); f.close();
}
static double survey_area_m2(){
  if(g_waypoint_count<3) return 0;
  double area=0;
  for(int i=0;i<g_waypoint_count;i++){int j=(i+1)%g_waypoint_count;area+=(double)WAYPOINTS[i].local_east*WAYPOINTS[j].local_north-(double)WAYPOINTS[j].local_east*WAYPOINTS[i].local_north;}
  return fabs(area)*0.5;
}
static void averaging_tick(){
  static uint32_t last_seq=0; if(!g_position_averaging||!GPS.valid||GPS.sequence==last_seq) return; last_seq=GPS.sequence;
  g_average_lat+=GPS.lat; g_average_lon+=GPS.lon; g_average_alt+=GPS.altitude; g_average_count++;
  if(g_average_count>=30){ append_waypoint("AVERAGED","Research","30-fix averaged position",g_average_lat/g_average_count,g_average_lon/g_average_count,g_average_alt/g_average_count,false); g_position_averaging=false; }
}
static void arrival_tick(){
  static uint32_t last=0;if(millis()-last<2000||!g_waypoint_count)return;last=millis();Waypoint&w=WAYPOINTS[g_waypoint_count-1];if(!w.arrived&&!w.geodetic&&local_distance_to(g_waypoint_count-1)<12.0f){w.arrived=true;rewrite_waypoints();}
}

// ---- shared time block (spec: TIME_CONTRACT.md) -----------------------------
#define TIME_SERIAL Serial
#define TIME_SRC_ID TIME_SRC_MODULE
// ===== SHARED TIME BLOCK v2 =================================================
//  Inlined verbatim into every app in this repo.  Spec + rationale:
//  TIME_CONTRACT.md.  Only the two lines above the block may differ per app:
//      #define TIME_SERIAL  Serial          // or USBSerial
//      #define TIME_SRC_ID  TIME_SRC_MODULE // or TIME_SRC_LAUNCHER
//
//  Namespace "rtcsave" -- the ONE place the watch's wall clock lives:
//      t    uint32  unix epoch seconds, UTC
//      gmt  int32   local offset from UTC, in SECONDS   (+28800 = UTC+8)
//      seq  uint32  write counter, ++ on every publish  (diagnostics)
//      man  uint8   1 = a human set this clock, 0 = machine (NTP/boot).  A boot
//                       refresh keeps it; a machine that lands a NEW epoch clears it.
//      src  uint8   0 = launcher, 1 = module            (diagnostics)
//
//  Two different things have to survive a reboot, and only one of them does:
//    * the EPOCH lives in the chip's RTC timer.  It survives every software
//      reset and every deep sleep, so a launcher <-> module reboot inherits
//      it for free -- but power loss wipes it, and then this snapshot is the
//      only thing between the user and "00:00".
//    * the TIMEZONE lives in the C library's TZ string, which is RAM-ONLY.
//      It dies on EVERY reboot and no app can inherit it from the previous
//      one.  NVS is therefore the offset's only home.
//  => frame: epoch is inherited through the RTC, backed up here.
//     timezone is stored here and re-applied by every single boot.
// ============================================================================
#define TIME_VALID_MIN      1700000000UL   // ~2023-11-14: "the RTC has been set"
#define TIME_ADOPT_SLACK_S  2L             // ignore sub-2 s snapshot/rtc skew
#define TIME_SRC_LAUNCHER   0
#define TIME_SRC_MODULE     1

static int32_t  g_tp_gmt   = 0;   // local offset from UTC, in seconds
static uint32_t g_tp_seq   = 0;   // last seq we read or wrote
static bool     g_tp_valid = false;   // a plausible wall clock is available

// Point the C library at the local UTC offset.  TZ is RAM-only, so this must
// run on EVERY boot in EVERY app or the watch silently renders UTC.
// POSIX counts the offset backwards from local to UTC, hence the minus.
static void time_apply_tz(int32_t gmt_sec) {
    long mins = -(gmt_sec / 60);
    long a    = labs(mins);
    char tz[24];
    snprintf(tz, sizeof(tz), "UTC%c%ld:%02ld", mins < 0 ? '-' : '+', a / 60, a % 60);
    setenv("TZ", tz, 1);
    tzset();
    g_tp_gmt = gmt_sec;
}

// Is the shared NVS snapshot a better clock than the one the chip is holding?
//   nothing saved yet            -> leave everything alone
//   chip has no valid clock      -> cold boot / dead battery: adopt the NVS
//   a human set it (man=1)       -> their correction wins in EITHER direction
//   snapshot runs ahead of the   -> somebody moved the clock forward: adopt
//   chip by more than the slack
//   otherwise                    -> the RTC is the live clock, keep it
//   Returns TIME_DEC_* below (uint8_t return keeps the Arduino .ino auto
//   prototype generator, which runs before this enum is declared, happy).
enum TimeDecision { TIME_DEC_NOTHING = 0, TIME_DEC_KEEP_RTC = 1, TIME_DEC_ADOPT = 2 };
static uint8_t time_decide(time_t rtc_now, uint32_t snap_t, uint8_t snap_man) {
    if (snap_t < TIME_VALID_MIN)                       return TIME_DEC_NOTHING;
    if (rtc_now < (time_t)TIME_VALID_MIN)              return TIME_DEC_ADOPT;
    if (snap_man && labs((long)snap_t - (long)rtc_now) > TIME_ADOPT_SLACK_S)
                                                       return TIME_DEC_ADOPT;
    if ((int32_t)(snap_t - (uint32_t)rtc_now) > TIME_ADOPT_SLACK_S)
                                                       return TIME_DEC_ADOPT;
    return TIME_DEC_KEEP_RTC;
}

// WRITE half of the contract: epoch + offset + provenance.  Call it after
// every NTP sync, after a manual set, before deep sleep, and on the way out of
// the app (before esp_restart()).
static void time_publish(const char *why, bool manual) {
    time_t now; time(&now);
    if (now < (time_t)TIME_VALID_MIN) return;      // RTC not set yet
    Preferences p;
    if (!p.begin("rtcsave", false)) return;
    uint32_t prev     = p.getULong("t", 0);
    uint8_t  prev_man = p.getUChar("man", 0);
    // A human's correction stays in force while the snapshot still describes
    // the same instant.  Every boot refreshes `t` by a second or so and must
    // not quietly demote the clock back to "machine" -- otherwise the man=1
    // rule above only survives a single reboot.  A machine that lands a
    // genuinely DIFFERENT epoch (an NTP sync) does take the clock over.
    bool machine_moved_it = !prev ||
                            labs((long)now - (long)prev) > TIME_ADOPT_SLACK_S;
    uint8_t man = (manual || (prev_man && !machine_moved_it)) ? 1 : 0;
    p.putULong("t",   (uint32_t)now);
    p.putLong ("gmt", g_tp_gmt);
    p.putUChar("man", man);
    p.putUChar("src", TIME_SRC_ID);
    g_tp_seq = p.getULong("seq", 0) + 1;           // read-modify-write, session open
    p.putULong("seq", g_tp_seq);
    p.end();
    TIME_SERIAL.printf("[time] publish t=%lu gmt=%ld seq=%lu man=%d (%s)\n",
                       (unsigned long)now, (long)g_tp_gmt,
                       (unsigned long)g_tp_seq, man, why);
}

// READ half: reconcile the EPOCH, then ALWAYS re-apply the TIMEZONE, then
// republish so the snapshot never lags the running clock by more than a boot.
// Call this at the very top of setup(), before anything that shows a time.
static void time_restore_from_nvs() {
    time_t rtc_now = 0; time(&rtc_now);

    uint32_t snap_t = 0, snap_seq = 0;
    int32_t  snap_gmt = 0;
    uint8_t  snap_man = 0;
    Preferences p;
    if (p.begin("rtcsave", true)) {
        snap_t   = p.getULong("t",   0);
        snap_gmt = p.getLong ("gmt", 0);
        snap_seq = p.getULong("seq", 0);
        snap_man = p.getUChar("man", 0);
        p.end();
    }
    if (snap_seq == 0 && snap_t) snap_seq = 1;     // pre-v2 snapshot

    uint8_t d = time_decide(rtc_now, snap_t, snap_man);
    if (d == TIME_DEC_ADOPT) {
        struct timeval tv = { (time_t)snap_t, 0 };
        settimeofday(&tv, NULL);                   // writes the chip RTC as well
        g_tp_valid = true;
        TIME_SERIAL.printf("[time] adopted NVS t=%lu (rtc was %ld man=%d)\n",
                           (unsigned long)snap_t, (long)rtc_now, snap_man);
    } else if (d == TIME_DEC_KEEP_RTC) {
        g_tp_valid = true;                         // v6.2 rule: never rewind a live RTC
        TIME_SERIAL.printf("[time] RTC live t=%ld -- NVS kept as fallback\n",
                           (long)rtc_now);
    } else {
        TIME_SERIAL.println("[time] no persisted clock -- waiting for NTP");
    }

    // TZ is the part that never survives a reboot: take it from here, always.
    if (snap_t) time_apply_tz(snap_gmt);
    g_tp_seq = snap_seq;
    time_publish("boot", false);                   // keep the snapshot current
}
// ===== END SHARED TIME BLOCK v2 =============================================
// The shared block owns the clock now; alias so the call site works.
static void restore_clock(){ time_restore_from_nvs(); }

static void save_clock(){
  time_publish("explorer", false);   // epoch + offset, one writer for "rtcsave"
  g_last_clock_save_ms=millis();
}

static void pump(){ lv_timer_handler(); }
static void fade_brightness(uint8_t target,uint32_t duration){
  int from=g_brightness;
  if(from==target) return;
  for(int i=1;i<=12;i++){
    int value=from+((int(target)-from)*i)/12;
    static_cast<Arduino_CO5300*>(gfx)->setBrightness((uint8_t)value);
    g_brightness=(uint8_t)value;
    pump(); delay(max(1,(int)duration/12));
  }
}

static void display_flush(lv_disp_drv_t *drv,const lv_area_t *area,lv_color_t *pixels){
  uint32_t w=area->x2-area->x1+1,h=area->y2-area->y1+1;
  gfx->draw16bitRGBBitmap(area->x1,area->y1,(uint16_t*)pixels,w,h);
  lv_disp_flush_ready(drv);
}

static void touch_read(lv_indev_drv_t*,lv_indev_data_t *data){
  int32_t x=FT3168->IIC_Read_Device_Value(FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_X);
  int32_t y=FT3168->IIC_Read_Device_Value(FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_Y);
  int fingers=FT3168->IIC_Read_Device_Value(FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_FINGER_NUMBER);
  bool pressed=fingers>0;
  static uint8_t zero_streak=0;
  if(pressed) zero_streak=0;
  else if(g_touch_active && ++zero_streak<3){ pressed=true; x=g_current_x; y=g_current_y; }
  if(pressed){
    data->state=LV_INDEV_STATE_PR; data->point.x=x; data->point.y=y;
    g_last_touch_ms=millis();
    if(!g_touch_active){ g_touch_active=true; g_start_x=x; g_start_y=y; g_start_ms=millis(); }
    g_current_x=x; g_current_y=y;
  } else {
    data->state=LV_INDEV_STATE_REL;
    if(g_touch_active){
      g_touch_active=false;
      int32_t dx=g_current_x-g_start_x,dy=g_current_y-g_start_y;
      if(millis()-g_start_ms<SWIPE_MAX_DURATION_MS && abs(dx)>SWIPE_THRESHOLD_PX && abs(dx)>abs(dy)){
        g_last_swipe_ms=millis();
        g_pending_swipe=(dx<0)?1:-1;
      }
      g_last_touch_ms=millis();
    }
  }
}

static lv_obj_t *label(lv_obj_t *parent,const char *text,const lv_font_t *font,uint32_t color,lv_align_t align,int x,int y){
  lv_obj_t *o=lv_label_create(parent); lv_label_set_text(o,text);
  lv_obj_set_style_text_font(o,font,0); lv_obj_set_style_text_color(o,lv_color_hex(color),0);
  lv_obj_align(o,align,x,y); return o;
}

static lv_obj_t *button(lv_obj_t *parent,const char *text,int x,int y,int w,int h,lv_event_cb_t cb,void *user){
  lv_obj_t *b=lv_btn_create(parent); lv_obj_set_pos(b,x,y); lv_obj_set_size(b,w,h);
  lv_obj_set_style_bg_color(b,lv_color_hex(COL_PANEL),0);lv_obj_set_style_bg_grad_color(b,lv_color_hex(0x0B1211),0);lv_obj_set_style_bg_grad_dir(b,LV_GRAD_DIR_VER,0);lv_obj_set_style_radius(b,15,0);
  lv_obj_set_style_border_width(b,1,0); lv_obj_set_style_border_color(b,lv_color_hex(COL_BORDER),0);
  lv_obj_set_style_shadow_color(b,lv_color_hex(COL_GLOW),0);lv_obj_set_style_shadow_width(b,8,0);lv_obj_set_style_shadow_opa(b,LV_OPA_20,0);
  lv_obj_set_style_bg_color(b,lv_color_hex(COL_GLOW),LV_STATE_PRESSED);lv_obj_set_style_border_color(b,lv_color_hex(ACCENT),LV_STATE_PRESSED);
  lv_obj_add_event_cb(b,cb,LV_EVENT_CLICKED,user);
  label(b,text,&lv_font_montserrat_14,COL_TEXT,LV_ALIGN_CENTER,0,0); return b;
}
static lv_obj_t *tool_tile(lv_obj_t*parent,size_t index,lv_event_cb_t cb){
  lv_obj_t*b=lv_btn_create(parent);lv_obj_set_size(b,177,88);lv_obj_set_style_bg_color(b,lv_color_hex(COL_PANEL),0);lv_obj_set_style_bg_grad_color(b,lv_color_hex(0x090E0D),0);lv_obj_set_style_bg_grad_dir(b,LV_GRAD_DIR_VER,0);lv_obj_set_style_radius(b,18,0);lv_obj_set_style_border_width(b,1,0);lv_obj_set_style_border_color(b,lv_color_hex(index%3==0?COL_GLOW:COL_BORDER),0);lv_obj_set_style_shadow_width(b,7,0);lv_obj_set_style_shadow_color(b,lv_color_hex(COL_GLOW),0);lv_obj_set_style_shadow_opa(b,LV_OPA_20,0);lv_obj_add_event_cb(b,cb,LV_EVENT_CLICKED,(void*)(uintptr_t)index);
  char code[8];snprintf(code,sizeof(code),"%02u",(unsigned)(index+1));label(b,code,&lv_font_montserrat_12,ACCENT,LV_ALIGN_TOP_RIGHT,-10,8);label(b,FEATURES[index].name,&lv_font_montserrat_14,COL_TEXT,LV_ALIGN_TOP_LEFT,10,10);lv_obj_t*s=label(b,FEATURES[index].summary,&lv_font_montserrat_12,COL_MUTED,LV_ALIGN_BOTTOM_LEFT,10,-9);lv_label_set_long_mode(s,LV_LABEL_LONG_DOT);lv_obj_set_width(s,150);return b;
}

static bool atomic_write(const String &path,const String &content){
  if(!g_sd_ok) return false;
  String tmp=path+".tmp";
  SD_MMC.remove(tmp.c_str());
  File f=SD_MMC.open(tmp.c_str(),FILE_WRITE);
  if(!f) return false;
  size_t expected=content.length(),written=f.print(content); f.flush(); f.close();
  if(written!=expected){ SD_MMC.remove(tmp.c_str()); return false; }
  SD_MMC.remove(path.c_str());
  return SD_MMC.rename(tmp.c_str(),path.c_str());
}

static void ensure_storage(){
  if(!g_sd_ok) return;
  if(!SD_MMC.exists("/WATCH")) SD_MMC.mkdir("/WATCH");
  if(!SD_MMC.exists(OS_FOLDER)) SD_MMC.mkdir(OS_FOLDER);
  for(size_t i=0;i<SUBDIR_COUNT;i++){
    String p=String(OS_FOLDER)+"/"+SUBDIRS[i];
    if(!SD_MMC.exists(p)) SD_MMC.mkdir(p.c_str());
  }
  String shared="/WATCH/FUSION_SHARED";
  if(!SD_MMC.exists(shared)) SD_MMC.mkdir(shared.c_str());
  const char *shared_dirs[]={"WAYPOINTS","ROUTES","LOCATIONS","DATASETS","NOTES","EVENTS","EXPORTS"};
  for(const char *d:shared_dirs){ String p=shared+"/"+d; if(!SD_MMC.exists(p)) SD_MMC.mkdir(p.c_str()); }
  String schema=String(OS_FOLDER)+"/.folder_version";
  if(!SD_MMC.exists(schema)) atomic_write(schema,"2\n");
  String config=String(OS_FOLDER)+"/config.json";
  if(!SD_MMC.exists(config)) atomic_write(config,String("{\n  \"schema\": 1,\n  \"environment\": \"")+OS_NAME+"\",\n  \"screen_timeout_ms\": 15000,\n  \"wifi_default\": false\n}\n");
  String map_help=String(OS_FOLDER)+"/MAPS/README.txt";
  if(!SD_MMC.exists(map_help.c_str())) atomic_write(map_help,"ExplorerOS lightweight map formats\nmap.csv: latitude,longitude,type,name\nrelative_map.csv: east_metres,north_metres,type,name\nTypes: road, path, landmark, water, hazard or custom.\nWithout GPS, ExplorerOS uses the relative map and an IMU-estimated trail.\nExisting files are never deleted during boot.\n");
  String boot_log=String(OS_FOLDER)+"/LOGS/boot.log";
  File log=SD_MMC.open(boot_log.c_str(),FILE_APPEND);
  if(log){ log.printf("%lu,%s,boot,sd=ok,imu=%d,pmu=%d\n",(unsigned long)time(nullptr),OS_NAME,g_has_imu,g_has_pmu); log.flush(); log.close(); }
}

static void filename_safe(char *text){for(char*p=text;*p;p++){if(!isalnum((unsigned char)*p)&&*p!='-'&&*p!='_')*p='_';}}
static void editor_done(lv_event_t *e){
  lv_event_code_t code=lv_event_get_code(e); if(code!=LV_EVENT_READY&&code!=LV_EVENT_CANCEL)return;
  if(code==LV_EVENT_READY&&g_edit_area){
    char text[64];snprintf(text,sizeof(text),"%s",lv_textarea_get_text(g_edit_area));clean_csv(text);
    if(text[0]){
      if(g_edit_target==EDIT_ROUTE_NAME&&g_route_path.length()){
        filename_safe(text);String next=String(OS_FOLDER)+"/ROUTES/"+text+".csv";if(SD_MMC.exists(next.c_str()))next=String(OS_FOLDER)+"/ROUTES/"+text+"_"+String(millis())+".csv";
        if(SD_MMC.rename(g_route_path.c_str(),next.c_str()))g_route_path=next;
      } else if(g_edit_target==EDIT_ROUTE_NOTE){
        if(g_route_state==ROUTE_ACTIVE)g_route_file.printf("# note,%lu,%s\n",(unsigned long)time(nullptr),text);else if(g_route_path.length()){File f=SD_MMC.open(g_route_path.c_str(),FILE_APPEND);if(f){f.printf("# note,%lu,%s\n",(unsigned long)time(nullptr),text);f.flush();f.close();}}add_field_note("ROUTE",text);
      } else if(g_edit_target==EDIT_WAYPOINT_NAME&&g_waypoint_count){strncpy(WAYPOINTS[g_waypoint_count-1].name,text,sizeof(WAYPOINTS[0].name)-1);WAYPOINTS[g_waypoint_count-1].name[sizeof(WAYPOINTS[0].name)-1]=0;rewrite_waypoints();}
      else if(g_edit_target==EDIT_WAYPOINT_NOTE&&g_waypoint_count){strncpy(WAYPOINTS[g_waypoint_count-1].note,text,sizeof(WAYPOINTS[0].note)-1);WAYPOINTS[g_waypoint_count-1].note[sizeof(WAYPOINTS[0].note)-1]=0;rewrite_waypoints();}
      else if(g_edit_target==EDIT_FIELD_NOTE)add_field_note("CUSTOM",text);
    }
  }
  g_edit_target=EDIT_NONE;g_edit_area=nullptr;if(g_editor){lv_obj_del_async(g_editor);g_editor=nullptr;}
}
static void open_editor(EditTarget target,const char *title,const char *initial=""){
  if(g_editor)return;
  g_edit_target=target;g_editor=lv_obj_create(lv_layer_top());lv_obj_set_size(g_editor,LCD_W,LCD_H);lv_obj_align(g_editor,LV_ALIGN_CENTER,0,0);lv_obj_set_style_bg_color(g_editor,lv_color_hex(0x050709),0);lv_obj_set_style_bg_opa(g_editor,LV_OPA_COVER,0);lv_obj_set_style_border_width(g_editor,0,0);
  label(g_editor,title,&lv_font_montserrat_18,ACCENT,LV_ALIGN_TOP_MID,0,14);g_edit_area=lv_textarea_create(g_editor);lv_obj_set_pos(g_edit_area,12,52);lv_obj_set_size(g_edit_area,386,78);lv_textarea_set_one_line(g_edit_area,true);lv_textarea_set_max_length(g_edit_area,43);lv_textarea_set_text(g_edit_area,initial);
  lv_obj_t *kb=lv_keyboard_create(g_editor);lv_obj_set_pos(kb,0,145);lv_obj_set_size(kb,LCD_W,357);lv_keyboard_set_textarea(kb,g_edit_area);lv_obj_add_event_cb(kb,editor_done,LV_EVENT_READY,nullptr);lv_obj_add_event_cb(kb,editor_done,LV_EVENT_CANCEL,nullptr);
}

static lv_obj_t *value_panel(lv_obj_t *parent,int y,int h){
  lv_obj_t *p=lv_obj_create(parent); lv_obj_set_pos(p,14,y); lv_obj_set_size(p,382,h);
  lv_obj_set_style_bg_color(p,lv_color_hex(COL_PANEL),0);lv_obj_set_style_bg_grad_color(p,lv_color_hex(0x080D0C),0);lv_obj_set_style_bg_grad_dir(p,LV_GRAD_DIR_VER,0);lv_obj_set_style_border_color(p,lv_color_hex(COL_BORDER),0);lv_obj_set_style_border_width(p,1,0);lv_obj_set_style_radius(p,18,0);lv_obj_set_style_shadow_color(p,lv_color_hex(COL_GLOW),0);lv_obj_set_style_shadow_width(p,10,0);lv_obj_set_style_shadow_opa(p,LV_OPA_20,0);return p;
}
static void module_back(lv_event_t*){
  if(millis()-g_last_swipe_ms<CLICK_AFTER_SWIPE_MS) return;
  g_module_id=-1;g_module_value=nullptr;g_module_aux=nullptr;g_map_box=nullptr;g_module_action=nullptr;g_compass_box=nullptr;g_compass_needle=nullptr;g_compass_readout=nullptr;
  lv_scr_load_anim(g_screens[1],LV_SCR_LOAD_ANIM_FADE_ON,160,0,true); g_module_screen=nullptr;
}
static lv_obj_t *module_begin(const char *title){
  lv_obj_t *s=lv_obj_create(nullptr); init_screen(s); g_module_screen=s;
  button(s,"<",12,12,48,42,module_back,nullptr);label(s,title,&lv_font_montserrat_22,COL_TEXT,LV_ALIGN_TOP_LEFT,74,17);lv_obj_t*p=lv_obj_create(s);lv_obj_set_pos(p,300,18);lv_obj_set_size(p,92,28);lv_obj_set_style_radius(p,LV_RADIUS_CIRCLE,0);lv_obj_set_style_bg_color(p,lv_color_hex(COL_GLOW),0);lv_obj_set_style_border_width(p,0,0);label(p,"OFFLINE",&lv_font_montserrat_12,ACCENT,LV_ALIGN_CENTER,0,0);
  lv_scr_load_anim(s,LV_SCR_LOAD_ANIM_FADE_ON,160,0,false); return s;
}
static void module_text(lv_obj_t *parent,const char *text,int y,int h=190){
  lv_obj_t *p=value_panel(parent,y,h); g_module_value=label(p,text,&lv_font_montserrat_14,COL_TEXT,LV_ALIGN_TOP_LEFT,14,12);
  lv_label_set_long_mode(g_module_value,LV_LABEL_LONG_WRAP); lv_obj_set_width(g_module_value,350);
}

static void map_render(){
  if(!g_map_box) return;
  lv_obj_clean(g_map_box);
  for(int x=0;x<350;x+=50){ lv_obj_t *v=lv_obj_create(g_map_box); lv_obj_remove_style_all(v); lv_obj_set_pos(v,x,0); lv_obj_set_size(v,1,245); lv_obj_set_style_bg_color(v,lv_color_hex(0x1B2730),0); lv_obj_set_style_bg_opa(v,LV_OPA_COVER,0); }
  for(int y=0;y<245;y+=50){ lv_obj_t *h=lv_obj_create(g_map_box); lv_obj_remove_style_all(h); lv_obj_set_pos(h,0,y); lv_obj_set_size(h,350,1); lv_obj_set_style_bg_color(h,lv_color_hex(0x1B2730),0); lv_obj_set_style_bg_opa(h,LV_OPA_COVER,0); }
  double centre_lat=g_map_center_lat,centre_lon=g_map_center_lon;
  float rotation=g_map_track_up?-navigation_heading():0;
  auto add_dot=[&](double lat,double lon,uint32_t color,int size){
    double north=(lat-centre_lat)*111320.0,east=(lon-centre_lon)*111320.0*cos(radians_d(centre_lat));
    float a=radians_d(rotation),rx=east*cos(a)-north*sin(a),ry=east*sin(a)+north*cos(a); int x=175+(int)(rx/g_map_metres_per_px),y=122-(int)(ry/g_map_metres_per_px);
    if(x<3||x>347||y<3||y>242)return;
    lv_obj_t *d=lv_obj_create(g_map_box); lv_obj_set_size(d,size,size); lv_obj_set_pos(d,x-size/2,y-size/2); lv_obj_set_style_radius(d,LV_RADIUS_CIRCLE,0); lv_obj_set_style_bg_color(d,lv_color_hex(color),0); lv_obj_set_style_border_width(d,0,0);
  };
  auto add_local=[&](float east,float north,uint32_t color,int size){float a=radians_d(rotation),de=east-g_map_center_east,dn=north-g_map_center_north,rx=de*cosf(a)-dn*sinf(a),ry=de*sinf(a)+dn*cosf(a);int x=175+(int)(rx/g_map_metres_per_px),y=122-(int)(ry/g_map_metres_per_px);if(x<3||x>347||y<3||y>242)return;lv_obj_t*d=lv_obj_create(g_map_box);lv_obj_set_size(d,size,size);lv_obj_set_pos(d,x-size/2,y-size/2);lv_obj_set_style_radius(d,LV_RADIUS_CIRCLE,0);lv_obj_set_style_bg_color(d,lv_color_hex(color),0);lv_obj_set_style_border_width(d,0,0);};
  for(int i=0;i<g_map_point_count;i++){uint32_t c=strcmp(MAP_POINTS[i].type,"hazard")==0?0xFF4040:strcmp(MAP_POINTS[i].type,"water")==0?0x3388DD:strcmp(MAP_POINTS[i].type,"path")==0?0x65D46E:strcmp(MAP_POINTS[i].type,"road")==0?0xA0A0A0:0x5599BB;if(MAP_POINTS[i].local)add_local(MAP_POINTS[i].lat,MAP_POINTS[i].lon,c,5);}
  int line_count=0;for(int i=0;i<g_track_count&&line_count<TRACK_RAM_POINTS;i++){int k=track_index_at(i);float a=radians_d(rotation),de=g_track_east[k]-g_map_center_east,dn=g_track_north[k]-g_map_center_north,rx=de*cosf(a)-dn*sinf(a),ry=de*sinf(a)+dn*cosf(a);g_map_line_points[line_count].x=max(-30,min(380,175+(int)(rx/g_map_metres_per_px)));g_map_line_points[line_count].y=max(-30,min(275,122-(int)(ry/g_map_metres_per_px)));line_count++;}if(line_count>1){lv_obj_t*trail=lv_line_create(g_map_box);lv_line_set_points(trail,g_map_line_points,line_count);lv_obj_set_style_line_color(trail,lv_color_hex(ACCENT),0);lv_obj_set_style_line_width(trail,3,0);lv_obj_set_style_line_rounded(trail,true,0);}
  int dot_step=max(1,g_track_count/60);for(int i=0;i<g_track_count;i+=dot_step){int k=track_index_at(i);add_local(g_track_east[k],g_track_north[k],0x9AF5B0,4);}
  for(int i=0;i<g_waypoint_count;i++)add_local(WAYPOINTS[i].local_east,WAYPOINTS[i].local_north,WAYPOINTS[i].favourite?0xFFD43B:0xFF8C42,8);
  add_local(g_camp_set?g_camp_east:0,g_camp_set?g_camp_north:0,COL_WARN,10);add_local(g_local_east_m,g_local_north_m,0xFFFFFF,12);label(g_map_box,g_map_track_up?"TRACK UP":"N",&lv_font_montserrat_12,ACCENT,LV_ALIGN_TOP_RIGHT,-8,6);
}
static void map_control(lv_event_t *e){
  int action=(int)(intptr_t)lv_event_get_user_data(e); double shift=g_map_metres_per_px*65.0;
  if(action==0)g_map_metres_per_px=max(0.5f,g_map_metres_per_px/2); else if(action==1)g_map_metres_per_px=min(500.0f,g_map_metres_per_px*2);
  else if(action==2)g_map_center_north+=shift;
  else if(action==3)g_map_center_north-=shift;
  else if(action==4)g_map_center_east-=shift;
  else if(action==5)g_map_center_east+=shift;
  else if(action==6){g_map_center_east=g_local_east_m;g_map_center_north=g_local_north_m;}
  else if(action==7)g_map_track_up=!g_map_track_up;
  map_render();
}
static void emergency_control(lv_event_t *e){
  int action=(int)(intptr_t)lv_event_get_user_data(e);
  if(action==0){if(g_emergency){lv_obj_del_async(g_emergency);g_emergency=nullptr;}g_signal_white=false;fade_brightness(active_brightness(),120);return;}
  g_signal_white=!g_signal_white;
  if(g_emergency){lv_obj_set_style_bg_color(g_emergency,lv_color_hex(g_signal_white?0xFFFFFF:0x140000),0);lv_obj_set_style_text_color(g_emergency,lv_color_hex(g_signal_white?0x000000:0xFFFFFF),LV_PART_MAIN);}
  fade_brightness(g_signal_white?255:active_brightness(),120);
}
static void show_emergency(){
  if(g_emergency)return;g_emergency=lv_obj_create(lv_layer_top());lv_obj_set_size(g_emergency,LCD_W,LCD_H);lv_obj_align(g_emergency,LV_ALIGN_CENTER,0,0);lv_obj_set_style_bg_color(g_emergency,lv_color_hex(0x140000),0);lv_obj_set_style_bg_opa(g_emergency,LV_OPA_COVER,0);lv_obj_set_style_border_width(g_emergency,0,0);
  label(g_emergency,"SOS / EMERGENCY",&lv_font_montserrat_28,0xFF5555,LV_ALIGN_TOP_MID,0,28);
  lv_obj_t*p=value_panel(g_emergency,82,246);lv_obj_set_style_bg_color(p,lv_color_hex(0x240606),0);lv_obj_t*t=label(p,"1. Call local emergency services if possible.\n\n2. Give location, injury, hazards and group size.\n\n3. Stay visible and conserve battery.\n\nSignal: 3 short, 3 long, 3 short.\nA steady white screen can aid visibility; it does not transmit a distress call.",&lv_font_montserrat_14,0xFFFFFF,LV_ALIGN_TOP_LEFT,14,12);lv_label_set_long_mode(t,LV_LABEL_LONG_WRAP);lv_obj_set_width(t,350);
  button(g_emergency,"STEADY SIGNAL",20,350,176,54,emergency_control,(void*)1);button(g_emergency,"CLOSE",214,350,176,54,emergency_control,(void*)0);
  label(g_emergency,"No GPS and no automatic emergency transmission",&lv_font_montserrat_12,0xFFB0B0,LV_ALIGN_BOTTOM_MID,0,-42);
}
static void guide_control(lv_event_t *e){
  int action=(int)(intptr_t)lv_event_get_user_data(e);
  if(action==4){show_emergency();return;}
  if(action==2)g_guide_checklist=!g_guide_checklist;
  else if(action==3&&g_guide_checklist){g_check_bits^=(1U<<g_check_index);Preferences p;p.begin("explorernav",false);p.putUChar("check",(uint8_t)g_check_bits);p.end();}
  else if(g_guide_checklist){int count=sizeof(CHECK_ITEMS)/sizeof(CHECK_ITEMS[0]);g_check_index=(g_check_index+action+count)%count;}
  else {int count=sizeof(GUIDE)/sizeof(GUIDE[0]);g_guide_index=(g_guide_index+action+count)%count;}
  update_module();
}
static void compass_visual_update(){if(!g_compass_needle)return;float h=navigation_heading(),a=radians_d(h);g_compass_points[0].x=66;g_compass_points[0].y=65;g_compass_points[1].x=66+(int)(44*sinf(a));g_compass_points[1].y=65-(int)(44*cosf(a));lv_line_set_points(g_compass_needle,g_compass_points,2);if(g_compass_readout){char text[32];snprintf(text,sizeof(text),"%03.0f deg",h);lv_label_set_text(g_compass_readout,text);}}
static void compass_control(lv_event_t *e){
  int action=(int)(intptr_t)lv_event_get_user_data(e);
  if(action==0){if(MAG.present){MAG.calibrating=!MAG.calibrating;if(MAG.calibrating){MAG.min_x=MAG.min_y=MAG.min_z=32767;MAG.max_x=MAG.max_y=MAG.max_z=-32768;}else mag_save_calibration();}else g_relative_heading=0;}
  else if(MAG.present){g_declination_deg=max(-30.0f,min(30.0f,g_declination_deg+action));Preferences p;p.begin("explorernav",false);p.putFloat("declination",g_declination_deg);p.end();}
  else g_relative_heading=fmodf(g_relative_heading+action+360.0f,360.0f);
}
static void route_control(lv_event_t *e){
  int action=(int)(intptr_t)lv_event_get_user_data(e);
  if(action==0){if(g_route_state==ROUTE_IDLE||g_route_state==ROUTE_SAVED)route_start();else route_pause_toggle();}
  else if(action==1)route_stop(); else if(action==2)save_current_waypoint("route waypoint");
}
static void waypoint_control(lv_event_t *e){
  int action=(int)(intptr_t)lv_event_get_user_data(e); if(action==0)save_current_waypoint(); else if(action==1)g_waypoint_category=(g_waypoint_category+1)%8;
  else if(action==2&&g_waypoint_count){WAYPOINTS[g_waypoint_count-1].favourite=!WAYPOINTS[g_waypoint_count-1].favourite;rewrite_waypoints();}
  else if(action==3&&g_waypoint_count){WAYPOINTS[g_waypoint_count-1].arrived=!WAYPOINTS[g_waypoint_count-1].arrived;rewrite_waypoints();}
}
static void note_control(lv_event_t *e){
  int action=(int)(intptr_t)lv_event_get_user_data(e); const char *tags[]={"OBSERVATION","HAZARD","SAMPLE","PHOTO"}; const char *notes[]={"Field observation marker","Hazard marker","Research sample marker","Photo reference marker"}; add_field_note(tags[action%4],notes[action%4]);
}
static void field_control(lv_event_t *e){
  int action=(int)(intptr_t)lv_event_get_user_data(e);
  if(action==-1)g_stride_m=max(0.30f,g_stride_m-0.05f);else if(action==1)g_stride_m=min(1.40f,g_stride_m+0.05f);else if(action==2){g_local_east_m=0;g_local_north_m=0;g_track_count=0;g_track_head=0;g_track_has_geo=false;g_map_center_east=0;g_map_center_north=0;}
  Preferences p;p.begin("explorernav",false);p.putFloat("stride",g_stride_m);p.end();update_module();
}
static void camp_control(lv_event_t*e){int action=(int)(intptr_t)lv_event_get_user_data(e);if(action==0){g_camp_set=true;g_camp_east=g_local_east_m;g_camp_north=g_local_north_m;save_current_waypoint("camp / safe return point");}else g_camp_set=false;Preferences p;p.begin("explorernav",false);p.putBool("camp",g_camp_set);p.putFloat("campe",g_camp_east);p.putFloat("campn",g_camp_north);p.end();update_module();}
static void adventure_timer_control(lv_event_t*e){int action=(int)(intptr_t)lv_event_get_user_data(e);if(action==0){if(g_adventure_timer_running){g_adventure_elapsed_ms+=millis()-g_adventure_timer_started;g_adventure_timer_running=false;}else{g_adventure_timer_running=true;g_adventure_timer_started=millis();g_hydration_due=millis()+(uint32_t)g_hydration_minutes*60000UL;}}else if(action==1){const uint16_t values[]={30,45,60,90};int next=0;for(int i=0;i<4;i++)if(values[i]==g_hydration_minutes)next=(i+1)%4;g_hydration_minutes=values[next];if(g_adventure_timer_running)g_hydration_due=millis()+(uint32_t)g_hydration_minutes*60000UL;}else if(action==2){const float goals[]={2,5,10,20};int next=0;for(int i=0;i<4;i++)if(fabsf(goals[i]-g_goal_km)<0.1f)next=(i+1)%4;g_goal_km=goals[next];}else if(action==3)g_lap_count++;Preferences p;p.begin("explorernav",false);p.putUChar("water",(uint8_t)g_hydration_minutes);p.putFloat("goal",g_goal_km);p.end();update_module();}
static void adventure_timer_tick(){if(!g_adventure_timer_running||!g_hydration_due||millis()<g_hydration_due)return;g_hydration_due=millis()+(uint32_t)g_hydration_minutes*60000UL;lv_obj_t*m=lv_msgbox_create(nullptr,"HYDRATION CHECK","Pause safely, drink water and reassess conditions. The reminder cannot measure hydration.",nullptr,true);lv_obj_center(m);}
static void power_control(lv_event_t*){ g_power_mode=(PowerMode)((g_power_mode+1)%7);Preferences p;p.begin("explorernav",false);p.putUChar("power",(uint8_t)g_power_mode);p.end();fade_brightness(active_brightness(),160); }
static void edit_control(lv_event_t *e){
  int action=(int)(intptr_t)lv_event_get_user_data(e);
  if(action==0)open_editor(EDIT_ROUTE_NAME,"Rename saved route","route");else if(action==1)open_editor(EDIT_ROUTE_NOTE,"Add route note","");
  else if(action==2&&g_waypoint_count)open_editor(EDIT_WAYPOINT_NAME,"Rename last waypoint",WAYPOINTS[g_waypoint_count-1].name);
  else if(action==3&&g_waypoint_count)open_editor(EDIT_WAYPOINT_NOTE,"Edit waypoint note",WAYPOINTS[g_waypoint_count-1].note);
  else if(action==4)open_editor(EDIT_FIELD_NOTE,"Custom field note","");
}

static bool export_relative_trail(){
  if(!g_sd_ok||g_track_count==0)return false;
  String final_path=String(OS_FOLDER)+"/EXPORTS/relative_trail_"+String((unsigned long)time(nullptr))+".csv";String tmp=final_path+".tmp";SD_MMC.remove(tmp.c_str());File f=SD_MMC.open(tmp.c_str(),FILE_WRITE);if(!f)return false;
  f.println("schema,source,unix,local_east_m,local_north_m,accuracy_note");
  for(int i=0;i<g_track_count;i++){int k=track_index_at(i);TrackPoint p=TRACK[k];f.printf("2,ExplorerOS,%lu,%.2f,%.2f,IMU_ESTIMATE_DRIFTS\n",(unsigned long)p.unix_time,g_track_east[k],g_track_north[k]);}
  f.flush();f.close();SD_MMC.remove(final_path.c_str());return SD_MMC.rename(tmp.c_str(),final_path.c_str());
}
static bool export_shared_waypoints(){
  if(!g_sd_ok)return false;
  String final_path="/WATCH/FUSION_SHARED/WAYPOINTS/explorer_waypoints.csv",tmp=final_path+".tmp";SD_MMC.remove(tmp.c_str());File f=SD_MMC.open(tmp.c_str(),FILE_WRITE);if(!f)return false;
  f.println("schema,source,name,category,local_east_m,local_north_m,unix,favourite,accuracy_note");for(int i=0;i<g_waypoint_count;i++){Waypoint&w=WAYPOINTS[i];f.printf("2,ExplorerOS,%s,%s,%.2f,%.2f,%lu,%d,IMU_ESTIMATE_DRIFTS\n",w.name,w.category,w.local_east,w.local_north,(unsigned long)w.unix_time,w.favourite?1:0);}f.flush();f.close();SD_MMC.remove(final_path.c_str());return SD_MMC.rename(tmp.c_str(),final_path.c_str());
}
static void export_control(lv_event_t *e){int action=(int)(intptr_t)lv_event_get_user_data(e);bool ok=action==0?export_relative_trail():export_shared_waypoints();if(g_module_aux)lv_label_set_text(g_module_aux,ok?"EXPORT COMPLETE":"NOTHING TO EXPORT / STORAGE ERROR");}

static void open_module(int id){
  g_module_id=id; lv_obj_t *s=module_begin(FEATURES[id].name);
  if(id==0){module_text(s,"Adventure guide loading...",72,235);button(s,"< PREV",16,322,112,46,guide_control,(void*)-1);button(s,"GUIDE / LIST",149,322,112,46,guide_control,(void*)2);button(s,"NEXT >",282,322,112,46,guide_control,(void*)1);button(s,"CHECK / UNCHECK",16,382,240,48,guide_control,(void*)3);button(s,"SOS",274,382,120,48,guide_control,(void*)4);}
  else if(id==1){ g_map_box=value_panel(s,70,255); lv_obj_set_style_pad_all(g_map_box,0,0); button(s,"+",14,338,44,44,map_control,(void*)0);button(s,"-",62,338,44,44,map_control,(void*)1);button(s,"N",110,338,44,44,map_control,(void*)2);button(s,"S",158,338,44,44,map_control,(void*)3);button(s,"W",206,338,44,44,map_control,(void*)4);button(s,"E",254,338,44,44,map_control,(void*)5);button(s,"HERE",302,338,94,44,map_control,(void*)6);button(s,"NORTH / TRACK UP",70,395,270,48,map_control,(void*)7);map_render(); }
  else if(id==2){g_compass_box=value_panel(s,66,132);lv_obj_t*arc=lv_arc_create(g_compass_box);lv_obj_set_pos(arc,10,8);lv_obj_set_size(arc,112,112);lv_arc_set_bg_angles(arc,0,360);lv_arc_set_angles(arc,0,360);lv_obj_remove_style(arc,nullptr,LV_PART_KNOB);lv_obj_set_style_arc_color(arc,lv_color_hex(COL_BORDER),LV_PART_MAIN);lv_obj_set_style_arc_color(arc,lv_color_hex(ACCENT),LV_PART_INDICATOR);lv_obj_set_style_arc_width(arc,3,LV_PART_MAIN);lv_obj_set_style_arc_width(arc,3,LV_PART_INDICATOR);g_compass_needle=lv_line_create(g_compass_box);lv_obj_set_style_line_color(g_compass_needle,lv_color_hex(COL_WARN),0);lv_obj_set_style_line_width(g_compass_needle,4,0);lv_obj_set_style_line_rounded(g_compass_needle,true,0);label(g_compass_box,"N",&lv_font_montserrat_12,ACCENT,LV_ALIGN_TOP_LEFT,62,2);label(g_compass_box,"GYRO-RELATIVE",&lv_font_montserrat_12,COL_MUTED,LV_ALIGN_TOP_LEFT,148,24);g_compass_readout=label(g_compass_box,"000 deg",&lv_font_montserrat_28,COL_TEXT,LV_ALIGN_TOP_LEFT,146,49);module_text(s,"Motion heading loading...",208,118);button(s,"SET NORTH",20,338,370,46,compass_control,(void*)0);button(s,"HEADING -5",20,398,175,46,compass_control,(void*)-5);button(s,"HEADING +5",215,398,175,46,compass_control,(void*)5);compass_visual_update();}
  else if(id==3){module_text(s,"Route ready",72,210);button(s,"START / PAUSE",16,298,180,48,route_control,(void*)0);button(s,"STOP + SAVE",214,298,180,48,route_control,(void*)1);button(s,"WAYPOINT",16,358,180,48,route_control,(void*)2);button(s,"ADD NOTE",214,358,180,48,edit_control,(void*)1);button(s,"RENAME SAVED ROUTE",70,418,270,46,edit_control,(void*)0);}
  else if(id==4){module_text(s,"No waypoints",72,210);button(s,"SAVE CURRENT",16,298,180,46,waypoint_control,(void*)0);button(s,"NEXT CATEGORY",214,298,180,46,waypoint_control,(void*)1);button(s,"FAV / UNFAV",16,354,180,46,waypoint_control,(void*)2);button(s,"ARRIVED",214,354,180,46,waypoint_control,(void*)3);button(s,"RENAME LAST",16,410,180,46,edit_control,(void*)2);button(s,"EDIT NOTE",214,410,180,46,edit_control,(void*)3);}
  else if(id==5){module_text(s,"Relative trail position loading...",72,330);}
  else if(id==6){module_text(s,"Markers are timestamped with measured or estimated position.",72,190);button(s,"OBSERVE",16,285,180,46,note_control,(void*)0);button(s,"HAZARD",214,285,180,46,note_control,(void*)1);button(s,"SAMPLE",16,341,180,46,note_control,(void*)2);button(s,"PHOTO REF",214,341,180,46,note_control,(void*)3);button(s,"CUSTOM NOTE",70,407,270,48,edit_control,(void*)4);}
  else if(id==7){module_text(s,"Survey uses relative trail markers.",72,250);button(s,"STRIDE -5cm",16,342,180,48,field_control,(void*)-1);button(s,"STRIDE +5cm",214,342,180,48,field_control,(void*)1);button(s,"RESET TRAIL ORIGIN",70,407,270,48,field_control,(void*)2);}
  else if(id==8){module_text(s,"Sensor status loading...",72,340);}
  else if(id==9){module_text(s,"Power mode loading...",72,260);button(s,"NEXT POWER MODE",20,365,370,58,power_control,nullptr);}
  else if(id==10){module_text(s,"Track and sensor log status loading...",72,260);button(s,"START / STOP SENSOR LOG",20,365,370,58,record_click,nullptr);}
  else if(id==11){module_text(s,"Exports are written atomically. Coordinates are relative estimates because this watch has no GPS.",72,210);button(s,"EXPORT RELATIVE TRAIL CSV",20,305,370,54,export_control,(void*)0);button(s,"SHARE TRAIL MARKER CSV",20,375,370,54,export_control,(void*)1);g_module_aux=label(s,"",&lv_font_montserrat_14,COL_MUTED,LV_ALIGN_BOTTOM_MID,0,-16);}
  else if(id==12){module_text(s,"Set a camp or safe-return marker before exploring.",72,250);button(s,"SET CAMP HERE",20,350,175,52,camp_control,(void*)0);button(s,"CLEAR CAMP",215,350,175,52,camp_control,(void*)1);label(s,"Relative guidance drifts. Use terrain, signs and a real compass.",&lv_font_montserrat_12,COL_WARN,LV_ALIGN_BOTTOM_MID,0,-28);}
  else if(id==13){module_text(s,"Adventure timer loading...",72,235);button(s,"START / STOP",16,326,180,46,adventure_timer_control,(void*)0);button(s,"WATER INTERVAL",214,326,180,46,adventure_timer_control,(void*)1);button(s,"DISTANCE GOAL",16,386,180,46,adventure_timer_control,(void*)2);button(s,"ADD LAP",214,386,180,46,adventure_timer_control,(void*)3);}
}

static void feature_click(lv_event_t *e){
  if(millis()-g_last_swipe_ms<CLICK_AFTER_SWIPE_MS) return;
  int i=(int)(intptr_t)lv_event_get_user_data(e); if(i>=0&&i<(int)FEATURE_COUNT)open_module(i);
}

static void set_screen(int next){
  if(next<0) next=2;
  if(next>2) next=0;
  if(next==g_screen) return;
  g_screen=next;
  lv_scr_load_anim(g_screens[g_screen],LV_SCR_LOAD_ANIM_FADE_ON,SCREEN_SWITCH_ANIM_MS,0,false);
  Preferences p; p.begin("fusionenv",false); p.putUChar("screen",g_screen); p.end();
}

static void record_click(lv_event_t*){
  if(millis()-g_last_swipe_ms<CLICK_AFTER_SWIPE_MS || !g_sd_ok) return;
  if(!g_recording){
    String folder=String(OS_FOLDER)+"/"+((FUSION_ENV==2)?"SENSOR_LOGS":(FUSION_ENV==3)?"DATASETS":"LOGS");
    String path=folder+"/session_"+String((unsigned long)time(nullptr))+"_"+String(millis())+".csv";
    g_log_file=SD_MMC.open(path.c_str(),FILE_WRITE);
    if(g_log_file){ g_log_file.println("unix_ms,ax_g,ay_g,az_g,battery_pct"); g_recording=true; }
  } else {
    g_log_file.flush(); g_log_file.close(); g_recording=false;
  }
  if(g_record_label) lv_label_set_text(g_record_label,g_recording?"STOP LOG":"START LOG");
}

static void init_screen(lv_obj_t *s){
  lv_obj_set_style_bg_color(s,lv_color_hex(0x020706),0);lv_obj_set_style_bg_grad_color(s,lv_color_hex(COL_BG),0);lv_obj_set_style_bg_grad_dir(s,LV_GRAD_DIR_VER,0);lv_obj_set_style_bg_opa(s,LV_OPA_COVER,0);
  lv_obj_clear_flag(s,LV_OBJ_FLAG_SCROLLABLE);
}

static void dashboard_action(lv_event_t*e){int action=(int)(intptr_t)lv_event_get_user_data(e);if(action==99)show_emergency();else open_module(action);}
static void build_dashboard(){
  lv_obj_t*s=lv_obj_create(nullptr);g_screens[0]=s;init_screen(s);
  label(s,"EXPLORER",&lv_font_montserrat_22,COL_TEXT,LV_ALIGN_TOP_LEFT,18,15);label(s,"FIELD//OS",&lv_font_montserrat_12,ACCENT,LV_ALIGN_TOP_LEFT,20,43);
  g_clock=label(s,"--:--",&lv_font_montserrat_48,COL_TEXT,LV_ALIGN_TOP_LEFT,16,62);g_battery=label(s,"BAT --%",&lv_font_montserrat_14,COL_MUTED,LV_ALIGN_TOP_RIGHT,-18,22);
  lv_obj_t*offline=lv_obj_create(s);lv_obj_set_pos(offline,300,58);lv_obj_set_size(offline,92,30);lv_obj_set_style_radius(offline,LV_RADIUS_CIRCLE,0);lv_obj_set_style_bg_color(offline,lv_color_hex(COL_GLOW),0);lv_obj_set_style_border_width(offline,0,0);label(offline,"OFFLINE",&lv_font_montserrat_12,ACCENT,LV_ALIGN_CENTER,0,0);
  lv_obj_t*card=value_panel(s,122,180);lv_obj_set_pos(card,14,122);lv_obj_set_size(card,382,180);label(card,"EXPEDITION STATUS",&lv_font_montserrat_12,ACCENT,LV_ALIGN_TOP_LEFT,15,12);g_dash_state=label(card,"READY TO EXPLORE",&lv_font_montserrat_18,COL_TEXT,LV_ALIGN_TOP_LEFT,15,36);
  lv_obj_t*rule=lv_obj_create(card);lv_obj_remove_style_all(rule);lv_obj_set_pos(rule,14,76);lv_obj_set_size(rule,352,1);lv_obj_set_style_bg_color(rule,lv_color_hex(COL_BORDER),0);lv_obj_set_style_bg_opa(rule,LV_OPA_COVER,0);
  label(card,"DISTANCE",&lv_font_montserrat_12,COL_MUTED,LV_ALIGN_TOP_LEFT,15,88);g_dash_distance=label(card,"0.00 km",&lv_font_montserrat_18,COL_TEXT,LV_ALIGN_TOP_LEFT,15,110);
  label(card,"HEADING",&lv_font_montserrat_12,COL_MUTED,LV_ALIGN_TOP_MID,0,88);g_dash_heading=label(card,"000 deg",&lv_font_montserrat_18,COL_TEXT,LV_ALIGN_TOP_MID,0,110);
  label(card,"STEPS",&lv_font_montserrat_12,COL_MUTED,LV_ALIGN_TOP_RIGHT,-15,88);g_dash_steps=label(card,"0",&lv_font_montserrat_18,COL_TEXT,LV_ALIGN_TOP_RIGHT,-15,110);
  button(s,"GUIDE",14,316,89,52,dashboard_action,(void*)0);button(s,"MAP",111,316,89,52,dashboard_action,(void*)1);button(s,"TRAIL",208,316,89,52,dashboard_action,(void*)3);button(s,"SOS",305,316,91,52,dashboard_action,(void*)99);
  button(s,"BACKTRACK TO CAMP",14,379,382,54,dashboard_action,(void*)12);
  label(s,"SWIPE FOR ALL TOOLS",&lv_font_montserrat_12,COL_MUTED,LV_ALIGN_BOTTOM_LEFT,18,-17);label(s,"HOLD BOOT: LAUNCHER",&lv_font_montserrat_12,0x59636D,LV_ALIGN_BOTTOM_RIGHT,-18,-17);
}

static void build_tools(){
  lv_obj_t *s=lv_obj_create(nullptr); g_screens[1]=s; init_screen(s);
  label(s,"FIELD KIT",&lv_font_montserrat_22,COL_TEXT,LV_ALIGN_TOP_LEFT,20,17);
  label(s,"14 OFFLINE TOOLS",&lv_font_montserrat_12,ACCENT,LV_ALIGN_TOP_RIGHT,-20,24);
  lv_obj_t *list=lv_obj_create(s); lv_obj_set_pos(list,12,58); lv_obj_set_size(list,386,402);
  lv_obj_set_style_bg_opa(list,LV_OPA_TRANSP,0); lv_obj_set_style_border_width(list,0,0);
  lv_obj_set_style_pad_all(list,4,0); lv_obj_set_style_pad_row(list,8,0); lv_obj_set_style_pad_column(list,8,0);
  lv_obj_set_flex_flow(list,LV_FLEX_FLOW_ROW_WRAP); lv_obj_set_scroll_dir(list,LV_DIR_VER);
  for(size_t i=0;i<FEATURE_COUNT;i++)tool_tile(list,i,feature_click);
  label(s,"dashboard  <  tools  >  system",&lv_font_montserrat_12,0x59636D,LV_ALIGN_BOTTOM_MID,0,-10);
}

static void launcher_click(lv_event_t*);
static void sleep_click(lv_event_t*);
static void build_system(){
  lv_obj_t *s=lv_obj_create(nullptr); g_screens[2]=s; init_screen(s);
  label(s,"SYSTEM",&lv_font_montserrat_22,ACCENT,LV_ALIGN_TOP_LEFT,20,18);
  char info[180]; snprintf(info,sizeof(info),"%s\nLVGL 8.4\nSD       %s\nQMI8658  %s\nAXP2101  %s\nWi-Fi    OFF",OS_NAME,g_sd_ok?"READY":"UNAVAILABLE",g_has_imu?"READY":"UNAVAILABLE",g_has_pmu?"READY":"UNAVAILABLE");
  lv_obj_t *card=lv_obj_create(s); lv_obj_set_pos(card,18,68); lv_obj_set_size(card,374,230);
  lv_obj_set_style_bg_color(card,lv_color_hex(COL_CARD),0); lv_obj_set_style_border_color(card,lv_color_hex(COL_BORDER),0); lv_obj_set_style_border_width(card,1,0); lv_obj_set_style_radius(card,20,0);
  label(card,info,&lv_font_montserrat_16,COL_TEXT,LV_ALIGN_TOP_LEFT,16,15);
  button(s,"SLEEP NOW",18,324,180,52,sleep_click,nullptr);
  button(s,"LAUNCHEROS",212,324,180,52,launcher_click,nullptr);
  label(s,"All OS data is stored under WATCH/",&lv_font_montserrat_14,COL_MUTED,LV_ALIGN_BOTTOM_MID,0,-68);
  label(s,"tools  <  system  >  dashboard",&lv_font_montserrat_12,0x59636D,LV_ALIGN_BOTTOM_MID,0,-18);
}

static void show_boot_error(const char *message){
  if(!g_screens[g_screen]) return;
  lv_obj_t *m=lv_msgbox_create(nullptr,"RETURN FAILED",message,nullptr,true);
  lv_obj_center(m); pump();
}

static void stop_and_save(){
  if(g_recording){ g_log_file.flush(); g_log_file.close(); g_recording=false; }
  if(g_route_state==ROUTE_ACTIVE||g_route_state==ROUTE_PAUSED) route_stop();
  if(MAG.calibrating){ MAG.calibrating=false; mag_save_calibration(); }
  save_clock();
  WiFi.disconnect(true,true); WiFi.mode(WIFI_OFF); btStop();
  Preferences p; p.begin("fusionenv",false); p.putUChar("screen",g_screen); p.end();
}

static void return_to_launcher(){
  time_publish("exit", false);   // hand the clock back to the launcher
  if(g_returning) return;
  g_returning=true;
  stop_and_save();
  lv_obj_t *cover=lv_obj_create(lv_layer_top()); lv_obj_set_size(cover,LCD_W,LCD_H); lv_obj_align(cover,LV_ALIGN_CENTER,0,0);
  lv_obj_set_style_bg_color(cover,lv_color_hex(COL_BG),0); lv_obj_set_style_bg_opa(cover,LV_OPA_COVER,0); lv_obj_set_style_border_width(cover,0,0);
  label(cover,"SAVING SESSION...",&lv_font_montserrat_22,ACCENT,LV_ALIGN_CENTER,0,-20);
  label(cover,"RETURNING TO LAUNCHEROS",&lv_font_montserrat_14,COL_MUTED,LV_ALIGN_CENTER,0,20); pump();
  const esp_partition_t *factory=esp_partition_find_first(ESP_PARTITION_TYPE_APP,ESP_PARTITION_SUBTYPE_APP_FACTORY,nullptr);
  if(!factory || esp_ota_set_boot_partition(factory)!=ESP_OK){
    lv_obj_del(cover); g_returning=false; show_boot_error("Factory LauncherOS partition was not found.\nNo alternate OTA slot was selected."); return;
  }
  fade_brightness(0,280); delay(80); esp_restart();
}

static void launcher_click(lv_event_t*){ if(millis()-g_last_swipe_ms>=CLICK_AFTER_SWIPE_MS) return_to_launcher(); }
static void screen_on(){
  if(!g_screen_off) return;
  g_screen_off=false;
  lv_obj_invalidate(lv_scr_act()); pump(); fade_brightness(active_brightness(),200);
  g_last_touch_ms=g_last_wake_ms=millis();
}
static void screen_off(){
  if(g_screen_off) return;
  g_screen_off=true; fade_brightness(0,220); FT3168->IIC_Interrupt_Flag=false;
}
static void sleep_click(lv_event_t*){
  if(millis()-g_last_swipe_ms<CLICK_AFTER_SWIPE_MS) return;
  stop_and_save(); fade_brightness(0,260);
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH,ESP_PD_OPTION_ON);
  esp_sleep_enable_ext1_wakeup(1ULL<<BOOT_BTN,ESP_EXT1_WAKEUP_ANY_LOW);
  esp_deep_sleep_start();
}

static void build_splash(){
  lv_obj_t *s=lv_obj_create(nullptr); init_screen(s); lv_scr_load(s);
  lv_obj_t *dot=lv_obj_create(s); lv_obj_set_size(dot,26,26); lv_obj_align(dot,LV_ALIGN_CENTER,0,-70);
  lv_obj_set_style_radius(dot,LV_RADIUS_CIRCLE,0); lv_obj_set_style_bg_color(dot,lv_color_hex(ACCENT),0); lv_obj_set_style_border_width(dot,0,0);
  label(s,OS_MARK,&lv_font_montserrat_28,COL_TEXT,LV_ALIGN_CENTER,0,-10);
  label(s,"F U S I O N   W A T C H",&lv_font_montserrat_14,ACCENT,LV_ALIGN_CENTER,0,32);
  label(s,"DISPLAY  TOUCH  STORAGE  SENSORS",&lv_font_montserrat_12,COL_MUTED,LV_ALIGN_BOTTOM_MID,0,-54);
  pump(); fade_brightness(DEFAULT_BRIGHTNESS,260);
}

static void coordinate_dms(double value,bool latitude,char *out,size_t size){
  char hemi=latitude?(value<0?'S':'N'):(value<0?'W':'E');double a=fabs(value);int deg=(int)a;double minutes=(a-deg)*60.0;int min=(int)minutes;double sec=(minutes-min)*60.0;snprintf(out,size,"%d deg %d' %.2f\" %c",deg,min,sec,hemi);
}

static void update_module(){
  if(g_module_id<0||!g_module_screen) return;
  char out[900]; out[0]=0;
  if(g_module_id==0){
    if(!g_guide_checklist){const GuideEntry&g=GUIDE[g_guide_index];snprintf(out,sizeof(out),"ADVENTURE GUIDE  %d / %d\n\n%s\n\n%s\n\nOffline quick reference. Follow local authorities, training and emergency services.",g_guide_index+1,(int)(sizeof(GUIDE)/sizeof(GUIDE[0])),g.title,g.body);}
    else {int n=snprintf(out,sizeof(out),"PRE-TRIP CHECKLIST\nSelect an item, then check/uncheck it.\n\n");for(int i=0;i<(int)(sizeof(CHECK_ITEMS)/sizeof(CHECK_ITEMS[0]))&&n<(int)sizeof(out)-48;i++)n+=snprintf(out+n,sizeof(out)-n,"%c [%c] %s\n",i==g_check_index?'>':' ',(g_check_bits&(1U<<i))?'x':' ',CHECK_ITEMS[i]);}
  } else if(g_module_id==2){
    float target_bearing=0,target_distance=0;if(g_waypoint_count){target_bearing=local_bearing_to(g_waypoint_count-1);target_distance=local_distance_to(g_waypoint_count-1);}
    float heading=navigation_heading(),error=fmodf(target_bearing-heading+540.0f,360.0f)-180.0f;
    snprintf(out,sizeof(out),"TARGET  %s\nBEARING %.0f deg   TURN %+.0f deg   DIST %.1f m\nTILT R/P %.0f / %.0f deg\nHeading drifts: face north and re-set often.",g_waypoint_count?WAYPOINTS[g_waypoint_count-1].name:"NONE",target_bearing,error,target_distance,atan2f(g_ay,g_az)*180/M_PI,atan2f(-g_ax,sqrtf(g_ay*g_ay+g_az*g_az))*180/M_PI);compass_visual_update();
  } else if(g_module_id==3){
    const char *state=g_route_state==ROUTE_ACTIVE?"ACTIVE":g_route_state==ROUTE_PAUSED?"PAUSED":g_route_state==ROUTE_SAVED?"SAVED":"READY"; uint32_t sec=route_elapsed_s(); float avg=sec?g_route_distance_m/sec*3.6f:0;
    snprintf(out,sizeof(out),"TRAIL       %s\nTIME        %02lu:%02lu:%02lu\nDISTANCE    %.3f km\nAVG SPEED   %.1f km/h\nMAX SPEED   %.1f km/h\nSTEPS       %lu\nPOINTS      %d\nSOURCE      IMU STEP ESTIMATE\nHEIGHT      unavailable\nFILE        %s\n\nDistance depends on stride and will drift.",state,(unsigned long)(sec/3600),(unsigned long)(sec/60%60),(unsigned long)(sec%60),g_route_distance_m/1000.0,avg,g_route_max_speed,(unsigned long)(g_step_count-g_last_route_step),g_track_count,g_route_path.length()?g_route_path.c_str():"not created");
  } else if(g_module_id==4){
    int n=snprintf(out,sizeof(out),"COUNT %d / %d\nNEXT CATEGORY  %s\n\n",g_waypoint_count,MAX_WAYPOINTS,WAYPOINT_CATEGORIES[g_waypoint_category]);
    for(int i=max(0,g_waypoint_count-5);i<g_waypoint_count&&n<(int)sizeof(out)-80;i++) n+=snprintf(out+n,sizeof(out)-n,"%s%s  %s%s\nE %.1f m / N %.1f m\n",WAYPOINTS[i].favourite?"* ":"",WAYPOINTS[i].name,WAYPOINTS[i].category,WAYPOINTS[i].arrived?" / ARRIVED":"",WAYPOINTS[i].local_east,WAYPOINTS[i].local_north);
  } else if(g_module_id==5){
    float target_bearing=0,target_distance=0;if(g_waypoint_count){target_bearing=local_bearing_to(g_waypoint_count-1);target_distance=local_distance_to(g_waypoint_count-1);}float pace=(g_last_step_gap_ms&&millis()-g_last_step_ms<4000)?g_stride_m/(g_last_step_gap_ms/1000.0f)*3.6f:0;
    snprintf(out,sizeof(out),"LOCAL EAST    %.1f m\nLOCAL NORTH   %.1f m\nSTEPS         %lu\nSTRIDE        %.2f m\nCURRENT PACE  %.1f km/h\nHEADING       %.1f deg\nTARGET        %s\nBEARING       %.1f deg\nDISTANCE      %.1f m\n\nAll position, pace and bearing values are IMU estimates. Heading and distance errors accumulate.",g_local_east_m,g_local_north_m,(unsigned long)g_step_count,g_stride_m,pace,navigation_heading(),g_waypoint_count?WAYPOINTS[g_waypoint_count-1].name:"NONE",target_bearing,target_distance);
  } else if(g_module_id==7){
    float d=0,b=0;if(g_waypoint_count>=2){float de=WAYPOINTS[1].local_east-WAYPOINTS[0].local_east,dn=WAYPOINTS[1].local_north-WAYPOINTS[0].local_north;d=hypotf(de,dn);b=fmodf(atan2f(de,dn)*180.0f/M_PI+360.0f,360.0f);}
    snprintf(out,sizeof(out),"SURVEY MARKERS %d\nP1 -> P2       %.2f m\nBEARING        %.1f deg\nPOLYGON AREA   %.2f m2\nSTRIDE         %.2f m\nCURRENT EAST   %.1f m\nCURRENT NORTH  %.1f m\n\nSave markers in trail order to measure a polygon. Values are relative step estimates, not cadastral measurements.",g_waypoint_count,d,b,survey_area_m2(),g_stride_m,g_local_east_m,g_local_north_m);
  } else if(g_module_id==8){
    snprintf(out,sizeof(out),"QMI8658       %s\nSAMPLE RATE   %.1f Hz\nSAMPLES       %lu\nACC X/Y/Z     %.3f  %.3f  %.3f g\nGYRO X/Y/Z    %.2f  %.2f  %.2f dps\nSTEP ENGINE   %s\nMOTION NORTH  %.1f deg\nMAGNETOMETER  %s\nBAROMETER     NOT FITTED\nGPS / GNSS    NOT FITTED\nBATTERY       %d%%",g_has_imu?"CONNECTED":"UNAVAILABLE",g_imu_hz,(unsigned long)g_imu_samples,g_ax,g_ay,g_az,g_gx,g_gy,g_gz,g_has_imu?"ACTIVE":"OFF",g_relative_heading,MAG.present?"OPTIONAL DETECTED":"NOT FITTED",g_has_pmu?g_pmu.getBatteryPercent():-1);
  } else if(g_module_id==9){
    snprintf(out,sizeof(out),"MODE\n%s\n\nIMU       %s\nMAP       OFFLINE\nWI-FI     OFF\nDISPLAY   15 s timeout\n\nFull: responsive trail tools\nTrail saver: less frequent disk flush\nLow trail: reduced screen updates\nMotion heading: direction tools\nOffline map: local reference map\nSensor sleep: 4 Hz IMU\nLow battery: minimum brightness",POWER_NAMES[g_power_mode],g_power_mode==POWER_SENSOR_SLEEP?"4 Hz":"~50 Hz");
  } else if(g_module_id==10){
    snprintf(out,sizeof(out),"SENSOR LOG   %s\nTRAIL LOG    %s\nTRAIL POINTS %d\nIMU SAMPLES  %lu\nSTORAGE      %s\n\nSensor CSV: time, acceleration, battery.\nTrail CSV: time, estimated speed, relative heading, east and north.\n\nAll trail coordinates are IMU estimates.",g_recording?"ACTIVE":"STOPPED",g_route_state==ROUTE_ACTIVE?"ACTIVE":"INACTIVE",g_track_count,(unsigned long)g_imu_samples,g_sd_ok?"READY":"UNAVAILABLE");
  } else if(g_module_id==12){
    float de=(g_camp_set?g_camp_east:0)-g_local_east_m,dn=(g_camp_set?g_camp_north:0)-g_local_north_m,d=hypotf(de,dn),bearing=fmodf(atan2f(de,dn)*180.0f/M_PI+360.0f,360.0f),turn=fmodf(bearing-navigation_heading()+540.0f,360.0f)-180.0f;snprintf(out,sizeof(out),"SAFE POINT     %s\nDISTANCE       %.1f m\nBEARING        %.1f deg\nTURN           %+.1f deg\nCURRENT E/N    %.1f / %.1f m\nCAMP E/N       %.1f / %.1f m\n\nFollow the breadcrumb map in reverse. IMU position and gyro heading accumulate error; this is an aid, not rescue navigation.",g_camp_set?"CAMP SAVED":"TRAIL ORIGIN",d,bearing,turn,g_local_east_m,g_local_north_m,g_camp_set?g_camp_east:0,g_camp_set?g_camp_north:0);
  } else if(g_module_id==13){
    uint32_t elapsed=(g_adventure_elapsed_ms+(g_adventure_timer_running?millis()-g_adventure_timer_started:0))/1000,remaining=g_adventure_timer_running&&g_hydration_due>millis()?(g_hydration_due-millis())/1000:0;float progress=g_goal_km>0?min(100.0f,(float)(g_route_distance_m/1000.0/g_goal_km*100.0)):0;snprintf(out,sizeof(out),"SESSION        %s\nSTOPWATCH      %02lu:%02lu:%02lu\nWATER CHECK    every %u min\nNEXT WATER     %02lu:%02lu\nDISTANCE GOAL  %.0f km\nGOAL PROGRESS  %.0f%%\nLAPS / MARKS   %lu\n\nReminders are prompts only; they cannot measure hydration or safety.",g_adventure_timer_running?"RUNNING":"STOPPED",(unsigned long)(elapsed/3600),(unsigned long)(elapsed/60%60),(unsigned long)(elapsed%60),g_hydration_minutes,(unsigned long)(remaining/60),(unsigned long)(remaining%60),g_goal_km,progress,(unsigned long)g_lap_count);
  } else return;
  if(g_module_value)lv_label_set_text(g_module_value,out);
}

static void update_live(){
  struct tm t; char clock_text[8]="--:--";
  if(getLocalTime(&t,5)) strftime(clock_text,sizeof(clock_text),"%H:%M",&t);
  if(g_clock) lv_label_set_text(g_clock,clock_text);
  int pct=g_has_pmu?g_pmu.getBatteryPercent():-1; char bat[20]; snprintf(bat,sizeof(bat),pct>=0?"BAT %d%%":"BAT --%%",pct); if(g_battery) lv_label_set_text(g_battery,bat);
  if(g_dash_state){const char*state=g_route_state==ROUTE_ACTIVE?"TRAIL RECORDING":g_route_state==ROUTE_PAUSED?"TRAIL PAUSED":g_camp_set?"CAMP MARKED":"READY TO EXPLORE";lv_label_set_text(g_dash_state,state);char metric[28];snprintf(metric,sizeof(metric),"%.2f km",g_route_distance_m/1000.0);lv_label_set_text(g_dash_distance,metric);snprintf(metric,sizeof(metric),"%03.0f deg",navigation_heading());lv_label_set_text(g_dash_heading,metric);snprintf(metric,sizeof(metric),"%lu",(unsigned long)g_step_count);lv_label_set_text(g_dash_steps,metric);}
  if(g_has_imu && g_imu.getDataReady()) g_imu.getAccelerometer(g_ax,g_ay,g_az);
  if(g_sensor){
    char out[80];
#if FUSION_ENV == 2
    float pace=(g_last_step_gap_ms&&millis()-g_last_step_ms<4000)?g_stride_m/(g_last_step_gap_ms/1000.0f)*3.6f:0;
    snprintf(out,sizeof(out),"TRAIL %.2f km  STEPS %lu\nIMU %s  PACE %.1f km/h",g_route_distance_m/1000.0,(unsigned long)g_step_count,g_has_imu?"LIVE":"OFF",pace);
#elif FUSION_ENV == 3
    float mag=sqrtf(g_ax*g_ax+g_ay*g_ay+g_az*g_az);
    snprintf(out,sizeof(out),"IMU %s   |a| %.3f g",g_has_imu?"CONNECTED":"OFF",mag);
#elif FUSION_ENV == 4
    time_t now=time(nullptr); double age=fmod((double)(now-947182440)/86400.0,29.53058867); if(age<0) age+=29.53058867;
    snprintf(out,sizeof(out),"MOON  age %.1f d  / approximate",age);
#else
    snprintf(out,sizeof(out),"LOCATION MODULE UNAVAILABLE");
#endif
    lv_label_set_text(g_sensor,out);
  }
  update_module();
}

static void hardware_init(){
  Serial.begin(115200); pinMode(BOOT_BTN,INPUT_PULLUP); pinMode(TP_INT,INPUT_PULLUP); btStop();
  gfx->begin(); gfx->fillScreen(0); static_cast<Arduino_CO5300*>(gfx)->setBrightness(0);
  pinMode(IIC_SCL,OUTPUT); for(int i=0;i<9;i++){ digitalWrite(IIC_SCL,LOW); delayMicroseconds(5); digitalWrite(IIC_SCL,HIGH); delayMicroseconds(5); }
  Wire.begin(IIC_SDA,IIC_SCL); Wire.setClock(400000);
  pinMode(TP_RESET,OUTPUT); digitalWrite(TP_RESET,LOW); delay(50); digitalWrite(TP_RESET,HIGH); delay(200); FT3168->begin();
  if(g_pmu.begin(Wire,AXP2101_SLAVE_ADDRESS,IIC_SDA,IIC_SCL)){ g_has_pmu=true; g_pmu.disableTSPinMeasure(); g_pmu.enableBattVoltageMeasure(); }
  if(g_imu.begin(Wire,QMI8658_L_SLAVE_ADDRESS,IIC_SDA,IIC_SCL)){ g_has_imu=true; g_imu.configAccelerometer(SensorQMI8658::ACC_RANGE_4G,SensorQMI8658::ACC_ODR_125Hz,SensorQMI8658::LPF_MODE_0); g_imu.enableAccelerometer(); g_imu.configGyroscope(SensorQMI8658::GYR_RANGE_1024DPS,SensorQMI8658::GYR_ODR_224_2Hz,SensorQMI8658::LPF_MODE_0); g_imu.enableGyroscope(); }
  mag_init();
#if EXPLORER_GPS_RX_PIN >= 0
  ExplorerGPS.begin(EXPLORER_GPS_BAUD,SERIAL_8N1,EXPLORER_GPS_RX_PIN,EXPLORER_GPS_TX_PIN); GPS.port=true;
#endif
  lv_init(); size_t pixels=LCD_W*60; g_lvbuf=(lv_color_t*)heap_caps_malloc(pixels*sizeof(lv_color_t),MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT); if(!g_lvbuf) g_lvbuf=(lv_color_t*)heap_caps_malloc(pixels*sizeof(lv_color_t),MALLOC_CAP_8BIT);
  lv_disp_draw_buf_init(&g_draw_buf,g_lvbuf,nullptr,pixels); lv_disp_drv_init(&g_disp_drv); g_disp_drv.flush_cb=display_flush; g_disp_drv.draw_buf=&g_draw_buf; g_disp_drv.hor_res=LCD_W; g_disp_drv.ver_res=LCD_H; lv_disp_drv_register(&g_disp_drv);
  lv_indev_drv_init(&g_indev_drv); g_indev_drv.type=LV_INDEV_TYPE_POINTER; g_indev_drv.read_cb=touch_read; lv_indev_drv_register(&g_indev_drv);
}

void setup(){
  hardware_init(); restore_clock(); build_splash(); delay(350);
  SD_MMC.setPins(SDMMC_CLK,SDMMC_CMD,SDMMC_DATA); g_sd_ok=SD_MMC.begin("/sdcard",true); ensure_storage();
  load_waypoints(); load_map_points();
  {Preferences nav;nav.begin("explorernav",true);g_power_mode=(PowerMode)min(6,(int)nav.getUChar("power",0));g_stride_m=max(0.30f,min(1.40f,nav.getFloat("stride",0.75f)));g_check_bits=nav.getUChar("check",0);g_camp_set=nav.getBool("camp",false);g_camp_east=nav.getFloat("campe",0);g_camp_north=nav.getFloat("campn",0);g_hydration_minutes=nav.getUChar("water",45);if(g_hydration_minutes<10)g_hydration_minutes=45;g_goal_km=max(1.0f,nav.getFloat("goal",5.0f));nav.end();}
  fade_brightness(active_brightness(),120);
  build_dashboard(); build_tools(); build_system();
  Preferences p; p.begin("fusionenv",true); g_screen=min(2,(int)p.getUChar("screen",0)); p.end();
  lv_scr_load_anim(g_screens[g_screen],LV_SCR_LOAD_ANIM_FADE_ON,260,0,false); g_last_touch_ms=millis(); update_live();
}

void loop(){
  static uint32_t last_tick=millis(),last_live=0,last_sample=0,last_map=0,boot_press=0;
  uint32_t now=millis();
#if !defined(LV_TICK_CUSTOM) || (LV_TICK_CUSTOM == 0)
  lv_tick_inc(now-last_tick);
#endif
  last_tick=now;pump();gps_tick();imu_tick();mag_tick();route_tick();averaging_tick();arrival_tick();adventure_timer_tick();
  if(g_pending_swipe && !g_editor){ int event=g_pending_swipe; g_pending_swipe=0; if(g_module_id>=0){if(event<0)module_back(nullptr);}else set_screen(g_screen+event); }
  if(now-last_live>=1000){ last_live=now; update_live(); }
  if(g_module_id==1&&now-last_map>=2000){last_map=now;map_render();}
  if(now-g_last_clock_save_ms>=300000UL) save_clock();
  if(g_recording && now-last_sample>=200){
    last_sample=now; int pct=g_has_pmu?g_pmu.getBatteryPercent():-1;
    g_log_file.printf("%llu,%.4f,%.4f,%.4f,%d\n",(unsigned long long)time(nullptr)*1000ULL,g_ax,g_ay,g_az,pct);
    if((now/1000)%5==0) g_log_file.flush();
  }
  bool grace=now-g_last_wake_ms<WAKE_GRACE_MS;
  if(g_touch_active || grace) g_last_touch_ms=now;
  if(!g_screen_off && now-g_last_touch_ms>SCREEN_OFF_TIMEOUT_MS) screen_off();
  else if(g_screen_off && digitalRead(TP_INT)==LOW) screen_on();
  if(digitalRead(BOOT_BTN)==LOW){
    if(!boot_press) boot_press=now;
    else if(!g_screen_off && now-boot_press>RETURN_HOLD_MS) return_to_launcher();
    else if(g_screen_off){ screen_on(); boot_press=now; }
  } else boot_press=0;
  delay(3);
}
