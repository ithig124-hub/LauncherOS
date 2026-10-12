// Fusion Watch standalone environment firmware
// Target: Waveshare ESP32-S3-Touch-AMOLED-2.06, Arduino, LVGL 8.4.x
// Build each environment as its own .ino by changing FUSION_ENV below.
// 1=TravelOS, 2=ExplorerOS, 3=ScienceOS, 4=AstroOS
#define FUSION_ENV 4

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

#define SCREEN_OFF_TIMEOUT_MS 15000UL
#define WAKE_GRACE_MS 2000UL
#define CLICK_AFTER_SWIPE_MS 350UL
#define SWIPE_THRESHOLD_PX 25
#define SWIPE_MAX_DURATION_MS 800UL
#define SCREEN_SWITCH_ANIM_MS 120
#define DEFAULT_BRIGHTNESS 180
#define RETURN_HOLD_MS 1500UL
#define COL_BG 0x01030A
#define COL_CARD 0x090E1C
#define COL_BORDER 0x243255
#define COL_TEXT 0xF4F7FA
#define COL_MUTED 0x8E9BB8
#define COL_PANEL 0x0D1428
#define COL_NEBULA 0x2A1F58
#define COL_CYAN 0x63D8FF
#define COL_RED 0xFF4A4A

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
static const char *SUBDIRS[]={"MAPS","ROUTES","WAYPOINTS","TRACKS","FIELD_NOTES","SURVEYS","SENSOR_LOGS","GPS_DATA","EXPORTS","LOGS","CACHE","SETTINGS"};
static const Feature FEATURES[]={
 {"GPS centre","Fix, coordinates, altitude, speed, accuracy and source"},
 {"Offline map","Pan, zoom, route, waypoint, north-up and track-up workspace"},
 {"Compass","Heading and bearing; unavailable without magnetometer hardware"},
 {"Route","Start, pause, resume, stop, save and annotate tracks"},
 {"Waypoints","Destination, meeting point, landmark and hazard records"},
 {"Dead reckon","Estimated motion from the IMU; uncertainty must be shown"},
 {"Field notes","Location-aware notes, readings, times and tags"},
 {"Survey","Distance, bearing, area, elevation and position averaging"},
 {"Sensors","Live QMI8658 accelerometer and hardware health"},
 {"Power modes","Full, GPS burst, low-power route and compass-only modes"},
 {"Track log","Versioned CSV sensor and route samples"},
 {"Export","Validated routes and waypoints for Fusion sharing"}
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
 {"Planets","Approximate position, visibility, brightness, distance and facts"},
 {"Moon","Phase, illumination, position, rise/set and next full/new moon"},
 {"Sun","Rise, set, noon, three twilight bands and day/night length"},
 {"Events","Phases, seasons, showers, reminders and imported event data"},
 {"Satellites","Passes require current imported orbital elements"},
 {"Orbit tools","Period, velocity, force, gravity, escape speed and Kepler guide"},
 {"Observe","Timestamped object, sky, equipment and rating log"},
 {"Locations","Saved observing sites for calculations"},
 {"Export","Versioned observations and locations for Fusion sharing"},
 {"Sky map","Live offline horizon map of visible bright stars"},
 {"Planner","Best visible targets ranked by altitude and brightness"},
 {"Night tools","Red torch, dark-adaptation timer and dim display mode"}
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
static lv_obj_t *g_overlay=nullptr;
static lv_obj_t *g_clock=nullptr,*g_battery=nullptr,*g_sensor=nullptr,*g_record_label=nullptr,*g_sky_summary=nullptr,*g_dash_moon_disc=nullptr,*g_dash_moon_shadow=nullptr;
static int g_screen=0;
static volatile int g_pending_swipe=0;
static int32_t g_start_x=0,g_start_y=0,g_current_x=0,g_current_y=0;
static uint32_t g_start_ms=0,g_last_touch_ms=0,g_last_wake_ms=0,g_last_swipe_ms=0;
static bool g_touch_active=false,g_screen_off=false,g_returning=false,g_recording=false;
static uint8_t g_brightness=0;
static File g_log_file;
static float g_ax=0,g_ay=0,g_az=0,g_gx=0,g_gy=0,g_gz=0;
static uint32_t g_last_clock_save_ms=0;

// ============================================================================
// Astro engine: offline calculations with manual location and RTC time
// ============================================================================
struct Star {const char*name;const char*constellation;float ra_h,dec_deg,mag;};
static const Star STARS[]={
 {"Sirius","Canis Major",6.7525f,-16.7161f,-1.46f},{"Canopus","Carina",6.3992f,-52.6957f,-0.74f},
 {"Arcturus","Bootes",14.2610f,19.1825f,-0.05f},{"Vega","Lyra",18.6156f,38.7837f,0.03f},
 {"Capella","Auriga",5.2782f,45.9980f,0.08f},{"Rigel","Orion",5.2423f,-8.2016f,0.13f},
 {"Procyon","Canis Minor",7.6550f,5.2250f,0.34f},{"Betelgeuse","Orion",5.9195f,7.4071f,0.42f},
 {"Achernar","Eridanus",1.6286f,-57.2368f,0.46f},{"Hadar","Centaurus",14.0637f,-60.3730f,0.61f},
 {"Altair","Aquila",19.8464f,8.8683f,0.77f},{"Acrux","Crux",12.4433f,-63.0991f,0.76f},
 {"Aldebaran","Taurus",4.5987f,16.5093f,0.86f},{"Antares","Scorpius",16.4901f,-26.4320f,0.91f},
 {"Spica","Virgo",13.4199f,-11.1613f,0.97f},{"Pollux","Gemini",7.7553f,28.0262f,1.14f}
};
static const char*STAR_NOTES[]={"Brightest night-sky star; A-type binary.","Bright southern supergiant.","Orange giant with high proper motion.","Blue-white star used in the Summer Triangle.","Nearby multiple-star system.","Blue supergiant in Orion.","Nearby F-type binary.","Red supergiant with variable brightness.","Hot blue star in Eridanus.","Blue giant in Centaurus.","Rapidly rotating Summer Triangle star.","Brightest star in Crux.","Orange giant marking the eye of Taurus.","Red supergiant in Scorpius.","Blue giant and close binary.","Orange giant in Gemini."};
struct Constellation {const char*name;const char*stars;const char*guide;};
static const Constellation CONSTELLATIONS[]={
 {"Orion","Betelgeuse - Bellatrix - Belt - Rigel - Saiph","Three close belt stars mark the centre. Follow the belt down toward Sirius."},
 {"Crux","Acrux - Mimosa - Gacrux - Delta Crucis","The long axis points roughly toward the south celestial pole."},
 {"Ursa Major","Dubhe - Merak - Phecda - Megrez - Alioth - Mizar - Alkaid","Extend the Dubhe-Merak line about five times to find Polaris."},
 {"Scorpius","Antares - Dschubba - Shaula - Lesath","Antares marks the red heart; the tail curves toward Shaula and Lesath."},
 {"Cassiopeia","Caph - Schedar - Navi - Ruchbah - Segin","A bright W shape in northern skies, opposite Ursa Major around Polaris."}
};
struct Planet {const char*name;float period_days,rotation_h,radius_km,gravity,orbit_au,phase0,typical_mag;const char*fact;};
static const Planet PLANETS[]={
 {"Mercury",87.969f,1407.6f,2439.7f,3.70f,0.387f,252.3f,-1.9f,"Fast inner planet; best near twilight."},
 {"Venus",224.701f,-5832.5f,6051.8f,8.87f,0.723f,181.9f,-4.7f,"Very bright; shows phases."},
 {"Mars",686.980f,24.62f,3389.5f,3.71f,1.524f,355.4f,-2.9f,"Reddish, with a 687-day orbit."},
 {"Jupiter",4332.59f,9.93f,69911.0f,24.79f,5.203f,34.4f,-2.9f,"Largest planet; four bright Galilean moons."},
 {"Saturn",10759.2f,10.7f,58232.0f,10.44f,9.537f,50.1f,0.5f,"Ring system is visible through a small telescope."},
 {"Uranus",30688.5f,-17.2f,25362.0f,8.69f,19.19f,314.1f,5.3f,"Ice giant; binocular/telescope target."},
 {"Neptune",60182.0f,16.1f,24622.0f,11.15f,30.07f,304.3f,7.8f,"Distant ice giant; telescope and chart required."}
};
struct Body {const char*name;double mu_km3_s2,radius_km,mass_kg;};
static const Body BODIES[]={
 {"Earth",398600.4418,6371.0,5.9722e24},{"Moon",4902.8001,1737.4,7.342e22},{"Mars",42828.375,3389.5,6.4171e23},{"Jupiter",126686534.0,69911.0,1.8982e27},{"Sun",132712440018.0,695700.0,1.9885e30}
};
struct Site {const char*name;float lat,lon;int tz;};
static const Site SITES[]={
 {"Perth",-31.9523f,115.8613f,8},{"Sydney",-33.8688f,151.2093f,10},{"London",51.5074f,-0.1278f,0},{"New York",40.7128f,-74.0060f,-5},{"Tokyo",35.6762f,139.6503f,9},{"Cape Town",-33.9249f,18.4241f,2}
};
static bool g_location_set=false;static float g_lat=0,g_lon=0;static int g_tz=0,g_site_index=0,g_location_field=0;
static int g_feature_id=-1,g_item_index=0,g_orbit_body=0;static float g_orbit_alt_km=400,g_sky_heading=0;static uint32_t g_star_favs=0;static bool g_event_reminder=false;
static lv_obj_t *g_overlay_value=nullptr,*g_overlay_status=nullptr,*g_constellation_box=nullptr,*g_sky_map_box=nullptr,*g_editor=nullptr,*g_obs_area=nullptr,*g_moon_disc=nullptr,*g_moon_shadow=nullptr,*g_red_overlay=nullptr,*g_dark_label=nullptr;
static uint32_t g_last_imu_ms=0;
static bool g_sky_map_track_up=false,g_dim_mode=false,g_dark_timer_running=false,g_dark_timer_complete=false;static uint32_t g_dark_timer_started=0;static lv_point_t g_sky_crosshair[2];
static void set_screen(int next);

static double norm_deg(double x){x=fmod(x,360.0);return x<0?x+360.0:x;}
static double julian_day(time_t stamp){return 2440587.5+(double)stamp/86400.0;}
static double gmst_deg(time_t stamp){double d=julian_day(stamp)-2451545.0;return norm_deg(280.46061837+360.98564736629*d);}
static void equatorial_to_horizontal(double ra_h,double dec_deg,time_t stamp,double &alt,double &az){
  double lst=norm_deg(gmst_deg(stamp)+g_lon),ha=norm_deg(lst-ra_h*15.0);if(ha>180)ha-=360;
  double h=ha*M_PI/180.0,d=dec_deg*M_PI/180.0,p=g_lat*M_PI/180.0;
  alt=asin(sin(d)*sin(p)+cos(d)*cos(p)*cos(h));
  az=atan2(-sin(h)*cos(d),sin(d)*cos(p)-cos(d)*sin(p)*cos(h));alt=alt*180.0/M_PI;az=norm_deg(az*180.0/M_PI);
}
static double moon_age_days(time_t stamp){double age=fmod((julian_day(stamp)-2451550.1),29.53058867);return age<0?age+29.53058867:age;}
static const char*moon_phase_name(double age){if(age<1.85||age>=27.68)return "New";if(age<5.54)return "Waxing crescent";if(age<9.23)return "First quarter";if(age<12.92)return "Waxing gibbous";if(age<16.61)return "Full";if(age<20.30)return "Waning gibbous";if(age<23.99)return "Last quarter";return "Waning crescent";}
static void solar_terms(time_t stamp,double &decl_deg,double &eq_minutes){
  double jd=julian_day(stamp),t=(jd-2451545.0)/36525.0,l0=norm_deg(280.46646+t*(36000.76983+t*0.0003032)),m=norm_deg(357.52911+t*(35999.05029-0.0001537*t));double e=0.016708634-t*(0.000042037+0.0000001267*t);double mr=m*M_PI/180.0,c=sin(mr)*(1.914602-t*(0.004817+0.000014*t))+sin(2*mr)*(0.019993-0.000101*t)+sin(3*mr)*0.000289;double lambda=norm_deg(l0+c-0.00569-0.00478*sin((125.04-1934.136*t)*M_PI/180.0));double eps=23.439291-0.0130042*t;decl_deg=asin(sin(eps*M_PI/180.0)*sin(lambda*M_PI/180.0))*180.0/M_PI;double y=tan(eps*M_PI/360.0);y*=y;eq_minutes=4.0*(y*sin(2*l0*M_PI/180.0)-2*e*sin(mr)+4*e*y*sin(mr)*cos(2*l0*M_PI/180.0)-0.5*y*y*sin(4*l0*M_PI/180.0)-1.25*e*e*sin(2*mr))*180.0/M_PI;
}
static bool sun_event(double zenith,time_t stamp,double &rise_h,double &set_h){double dec,eq;solar_terms(stamp,dec,eq);double p=g_lat*M_PI/180.0,d=dec*M_PI/180.0,cs=(cos(zenith*M_PI/180.0)-sin(p)*sin(d))/(cos(p)*cos(d));if(cs<-1||cs>1)return false;double ha=acos(cs)*180.0/M_PI;double noon=720.0-4.0*g_lon-eq+g_tz*60.0;rise_h=(noon-4*ha)/60.0;set_h=(noon+4*ha)/60.0;while(rise_h<0)rise_h+=24;while(rise_h>=24)rise_h-=24;while(set_h<0)set_h+=24;while(set_h>=24)set_h-=24;return true;}
static void format_hour(double h,char*out,size_t n){int hh=(int)h,mm=(int)round((h-hh)*60);if(mm>=60){mm-=60;hh=(hh+1)%24;}snprintf(out,n,"%02d:%02d",hh,mm);}
// Index-based signatures avoid Arduino's .ino prototype generator moving a
// Planet-dependent declaration ahead of the Planet definition.
static void planet_approx(int planet_index,time_t stamp,double &ra_h,double &dec_deg){
  const Planet&p=PLANETS[planet_index];
  double d=julian_day(stamp)-2451545.0,lp=norm_deg(p.phase0+360.0*d/p.period_days),le=norm_deg(100.46+360.0*d/365.256);double xp=p.orbit_au*cos(lp*M_PI/180.0)-cos(le*M_PI/180.0),yp=p.orbit_au*sin(lp*M_PI/180.0)-sin(le*M_PI/180.0);double lon=atan2(yp,xp),eps=23.4393*M_PI/180.0;double x=cos(lon),y=sin(lon)*cos(eps),z=sin(lon)*sin(eps);ra_h=norm_deg(atan2(y,x)*180.0/M_PI)/15.0;dec_deg=atan2(z,sqrt(x*x+y*y))*180.0/M_PI;
}
static double planet_distance_au(int planet_index,time_t stamp){const Planet&p=PLANETS[planet_index];double d=julian_day(stamp)-2451545.0,lp=norm_deg(p.phase0+360.0*d/p.period_days)*M_PI/180.0,le=norm_deg(100.46+360.0*d/365.256)*M_PI/180.0;return sqrt(p.orbit_au*p.orbit_au+1.0-2*p.orbit_au*cos(lp-le));}
static void moon_equatorial(time_t stamp,double &ra_h,double &dec_deg){double d=julian_day(stamp)-2451545.0,l=norm_deg(218.316+13.176396*d),m=norm_deg(134.963+13.064993*d),f=norm_deg(93.272+13.229350*d);double lon=(l+6.289*sin(m*M_PI/180.0))*M_PI/180.0,lat=5.128*sin(f*M_PI/180.0)*M_PI/180.0,eps=23.4393*M_PI/180.0;double x=cos(lon)*cos(lat),y=sin(lon)*cos(lat)*cos(eps)-sin(lat)*sin(eps),z=sin(lon)*cos(lat)*sin(eps)+sin(lat)*cos(eps);ra_h=norm_deg(atan2(y,x)*180.0/M_PI)/15.0;dec_deg=asin(z)*180.0/M_PI;}
static void moon_rise_set(time_t now,char*rise,size_t rn,char*set,size_t sn){snprintf(rise,rn,"--");snprintf(set,sn,"--");if(!g_location_set)return;time_t local_now=now+g_tz*3600;struct tm day;gmtime_r(&local_now,&day);day.tm_hour=day.tm_min=day.tm_sec=0;time_t start=mktime(&day)-g_tz*3600;double prev_alt,az,ra,dec;moon_equatorial(start,ra,dec);equatorial_to_horizontal(ra,dec,start,prev_alt,az);for(int m=10;m<=1440;m+=10){time_t t=start+m*60;double alt;moon_equatorial(t,ra,dec);equatorial_to_horizontal(ra,dec,t,alt,az);if(prev_alt<0&&alt>=0&&rise[0]=='-')format_hour(m/60.0,rise,rn);if(prev_alt>=0&&alt<0&&set[0]=='-')format_hour(m/60.0,set,sn);prev_alt=alt;}}
static bool parse_tle(char*name,size_t name_n,double &epoch,double &inc,double &raan,double &ecc,double &argp,double &mean_anom,double &mean_motion){
  if(!g_sd_ok)return false;String path=String(OS_FOLDER)+"/SATELLITES/tle.txt";File f=SD_MMC.open(path.c_str(),FILE_READ);if(!f)return false;char l1[96]={0},l2[96]={0};size_t a=f.readBytesUntil('\n',name,name_n-1),b=f.readBytesUntil('\n',l1,sizeof(l1)-1),c=f.readBytesUntil('\n',l2,sizeof(l2)-1);f.close();name[a]=0;l1[b]=0;l2[c]=0;if(l1[0]!='1'||l2[0]!='2')return false;
  char ep[16]={0},ec[10]={0};if(strlen(l1)<32)return false;memcpy(ep,l1+18,14);ep[14]=0;int yy=(ep[0]-'0')*10+ep[1]-'0';double day=atof(ep+2);int year=yy>=57?1900+yy:2000+yy;struct tm jan={};jan.tm_year=year-1900;jan.tm_mon=0;jan.tm_mday=1;jan.tm_isdst=0;setenv("TZ","UTC0",1);tzset();epoch=(double)mktime(&jan)+(day-1.0)*86400.0;
  if(sscanf(l2,"%*d %*d %lf %lf %9s %lf %lf %lf",&inc,&raan,ec,&argp,&mean_anom,&mean_motion)!=6)return false;ecc=atof(ec);if(ecc>=1.0)ecc*=1e-7;return mean_motion>0&&ecc>=0&&ecc<1;
}
static void tle_altaz(double stamp,double epoch,double inc,double raan,double ecc,double argp,double mean_anom,double mean_motion,double &alt,double &az){
  const double mu=398600.4418,n=mean_motion*2*M_PI/86400.0,a=cbrt(mu/(n*n));double m=norm_deg(mean_anom+(stamp-epoch)*mean_motion*360.0/86400.0)*M_PI/180.0,e=m;for(int i=0;i<8;i++)e-=(e-ecc*sin(e)-m)/(1-ecc*cos(e));double x=a*(cos(e)-ecc),y=a*sqrt(1-ecc*ecc)*sin(e);double O=raan*M_PI/180.0,I=inc*M_PI/180.0,W=argp*M_PI/180.0;double x1=(cos(O)*cos(W)-sin(O)*sin(W)*cos(I))*x+(-cos(O)*sin(W)-sin(O)*cos(W)*cos(I))*y;double y1=(sin(O)*cos(W)+cos(O)*sin(W)*cos(I))*x+(-sin(O)*sin(W)+cos(O)*cos(W)*cos(I))*y;double z1=sin(W)*sin(I)*x+cos(W)*sin(I)*y;double th=gmst_deg((time_t)stamp)*M_PI/180.0,xe=cos(th)*x1+sin(th)*y1,ye=-sin(th)*x1+cos(th)*y1,ze=z1;double p=g_lat*M_PI/180.0,l=g_lon*M_PI/180.0,r=6371.0,xo=r*cos(p)*cos(l),yo=r*cos(p)*sin(l),zo=r*sin(p),dx=xe-xo,dy=ye-yo,dz=ze-zo;double east=-sin(l)*dx+cos(l)*dy,north=-sin(p)*cos(l)*dx-sin(p)*sin(l)*dy+cos(p)*dz,up=cos(p)*cos(l)*dx+cos(p)*sin(l)*dy+sin(p)*dz;alt=atan2(up,sqrt(east*east+north*north))*180.0/M_PI;az=norm_deg(atan2(east,north)*180.0/M_PI);
}
static bool next_sat_pass(time_t now,double epoch,double inc,double raan,double ecc,double argp,double mean_anom,double mean_motion,time_t &rise,time_t &peak,time_t &set,float &peak_alt){
  bool inside=false;peak_alt=-90;rise=peak=set=0;for(int s=0;s<=86400;s+=30){double alt,az;tle_altaz((double)now+s,epoch,inc,raan,ecc,argp,mean_anom,mean_motion,alt,az);if(!inside&&alt>=10){inside=true;rise=now+s;peak=rise;peak_alt=alt;}if(inside&&alt>peak_alt){peak_alt=alt;peak=now+s;}if(inside&&alt<10){set=now+s;return true;}}return false;
}
static void save_location(){Preferences p;p.begin("astro",false);p.putBool("loc",g_location_set);p.putFloat("lat",g_lat);p.putFloat("lon",g_lon);p.putInt("tz",g_tz);p.end();}

static void restore_clock(){
  time_t current=time(nullptr);
  if(current>=1700000000) return;
  Preferences p; p.begin("rtcsave",true);
  uint32_t saved=p.getULong("t",0); long gmt=p.getLong("gmt",0); p.end();
  if(saved>=1700000000){ struct timeval tv={(time_t)saved,0}; settimeofday(&tv,nullptr); setenv("TZ","UTC0",1); tzset(); (void)gmt; }
}

static void save_clock(){
  time_t current=time(nullptr); if(current<1700000000) return;
  Preferences p; p.begin("rtcsave",false); p.putULong("t",(uint32_t)current); p.end();
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
  lv_obj_set_style_bg_color(b,lv_color_hex(COL_PANEL),0);lv_obj_set_style_bg_grad_color(b,lv_color_hex(0x070B16),0);lv_obj_set_style_bg_grad_dir(b,LV_GRAD_DIR_VER,0);lv_obj_set_style_radius(b,15,0);
  lv_obj_set_style_border_width(b,1,0); lv_obj_set_style_border_color(b,lv_color_hex(COL_BORDER),0);
  lv_obj_set_style_shadow_color(b,lv_color_hex(COL_NEBULA),0);lv_obj_set_style_shadow_width(b,9,0);lv_obj_set_style_shadow_opa(b,LV_OPA_30,0);lv_obj_set_style_bg_color(b,lv_color_hex(COL_NEBULA),LV_STATE_PRESSED);lv_obj_set_style_border_color(b,lv_color_hex(ACCENT),LV_STATE_PRESSED);
  lv_obj_add_event_cb(b,cb,LV_EVENT_CLICKED,user);
  label(b,text,&lv_font_montserrat_14,COL_TEXT,LV_ALIGN_CENTER,0,0); return b;
}
static lv_obj_t*tool_tile(lv_obj_t*parent,size_t index,lv_event_cb_t cb){lv_obj_t*b=lv_btn_create(parent);lv_obj_set_size(b,177,88);lv_obj_set_style_bg_color(b,lv_color_hex(COL_PANEL),0);lv_obj_set_style_bg_grad_color(b,lv_color_hex(0x070B16),0);lv_obj_set_style_bg_grad_dir(b,LV_GRAD_DIR_VER,0);lv_obj_set_style_radius(b,18,0);lv_obj_set_style_border_width(b,1,0);lv_obj_set_style_border_color(b,lv_color_hex(index%4==0?COL_NEBULA:COL_BORDER),0);lv_obj_set_style_shadow_color(b,lv_color_hex(COL_NEBULA),0);lv_obj_set_style_shadow_width(b,8,0);lv_obj_set_style_shadow_opa(b,LV_OPA_30,0);lv_obj_add_event_cb(b,cb,LV_EVENT_CLICKED,(void*)(uintptr_t)index);char number[8];snprintf(number,sizeof(number),"%02u",(unsigned)(index+1));label(b,number,&lv_font_montserrat_12,ACCENT,LV_ALIGN_TOP_RIGHT,-10,8);label(b,FEATURES[index].name,&lv_font_montserrat_14,COL_TEXT,LV_ALIGN_TOP_LEFT,10,10);lv_obj_t*s=label(b,FEATURES[index].summary,&lv_font_montserrat_12,COL_MUTED,LV_ALIGN_BOTTOM_LEFT,10,-9);lv_label_set_long_mode(s,LV_LABEL_LONG_DOT);lv_obj_set_width(s,150);return b;}

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
  String sat_help=String(OS_FOLDER)+"/SATELLITES/README.txt";
  if(!SD_MMC.exists(sat_help.c_str()))atomic_write(sat_help,"Place a current three-line element set in tle.txt:\nline 1 object name\nline 2 TLE line beginning 1\nline 3 TLE line beginning 2\nAstroOS uses an approximate two-body propagator, not full SGP4. Refresh TLE data often.\n");
  String cat_help=String(OS_FOLDER)+"/CATALOGUES/README.txt";
  if(!SD_MMC.exists(cat_help.c_str()))atomic_write(cat_help,"Built-in bright-star catalogue is offline. Imported catalogues should use name,ra_hours,dec_degrees,magnitude,constellation.\n");
  String boot_log=String(OS_FOLDER)+"/LOGS/boot.log";
  File log=SD_MMC.open(boot_log.c_str(),FILE_APPEND);
  if(log){ log.printf("%lu,%s,boot,sd=ok,imu=%d,pmu=%d\n",(unsigned long)time(nullptr),OS_NAME,g_has_imu,g_has_pmu); log.flush(); log.close(); }
}

static void close_overlay(lv_event_t*){
  if(millis()-g_last_swipe_ms<CLICK_AFTER_SWIPE_MS) return;
  if(g_overlay){lv_obj_del_async(g_overlay);g_overlay=nullptr;}
  g_overlay_value=nullptr;g_overlay_status=nullptr;g_constellation_box=nullptr;g_sky_map_box=nullptr;g_dark_label=nullptr;g_feature_id=-1;
}

static void constellation_draw(){
  if(!g_constellation_box)return;static const int16_t shape[5][5][2]={{{15,75},{55,35},{100,52},{145,22},{195,72}},{{25,70},{70,18},{105,72},{145,34},{200,68}},{{20,75},{65,52},{108,68},{145,42},{205,18}},{{20,22},{60,44},{100,34},{145,68},{205,80}},{{18,25},{62,65},{105,28},{150,65},{205,25}}};static lv_point_t pts[5];lv_obj_clean(g_constellation_box);for(int i=0;i<5;i++){pts[i].x=shape[g_item_index%5][i][0]+48;pts[i].y=shape[g_item_index%5][i][1]+5;lv_obj_t*d=lv_obj_create(g_constellation_box);lv_obj_set_pos(d,pts[i].x-3,pts[i].y-3);lv_obj_set_size(d,i==2?8:6,i==2?8:6);lv_obj_set_style_radius(d,LV_RADIUS_CIRCLE,0);lv_obj_set_style_bg_color(d,lv_color_hex(i==2?0xFFF1A8:0xFFFFFF),0);lv_obj_set_style_border_width(d,0,0);}lv_obj_t*line=lv_line_create(g_constellation_box);lv_line_set_points(line,pts,5);lv_obj_set_style_line_color(line,lv_color_hex(0x59636D),0);lv_obj_set_style_line_width(line,2,0);
}
static void sky_map_draw(){
  if(!g_sky_map_box)return;lv_obj_clean(g_sky_map_box);
  if(!g_location_set){label(g_sky_map_box,"SET A MANUAL LOCATION\nTO BUILD THE LIVE SKY MAP",&lv_font_montserrat_16,COL_MUTED,LV_ALIGN_CENTER,0,0);return;}
  lv_obj_t*ring=lv_arc_create(g_sky_map_box);lv_obj_set_pos(ring,81,12);lv_obj_set_size(ring,220,220);lv_arc_set_bg_angles(ring,0,360);lv_arc_set_angles(ring,0,360);lv_obj_remove_style(ring,nullptr,LV_PART_KNOB);lv_obj_set_style_arc_color(ring,lv_color_hex(COL_BORDER),LV_PART_MAIN);lv_obj_set_style_arc_color(ring,lv_color_hex(COL_NEBULA),LV_PART_INDICATOR);lv_obj_set_style_arc_width(ring,2,LV_PART_MAIN);lv_obj_set_style_arc_width(ring,2,LV_PART_INDICATOR);
  label(g_sky_map_box,"N",&lv_font_montserrat_12,ACCENT,LV_ALIGN_TOP_MID,0,4);label(g_sky_map_box,"E",&lv_font_montserrat_12,COL_MUTED,LV_ALIGN_RIGHT_MID,-58,-5);label(g_sky_map_box,"S",&lv_font_montserrat_12,COL_MUTED,LV_ALIGN_BOTTOM_MID,0,-16);label(g_sky_map_box,"W",&lv_font_montserrat_12,COL_MUTED,LV_ALIGN_LEFT_MID,58,-5);
  time_t now=time(nullptr);float rotation=g_sky_map_track_up?-g_sky_heading:0;for(int i=0;i<(int)(sizeof(STARS)/sizeof(STARS[0]));i++){double alt,az;equatorial_to_horizontal(STARS[i].ra_h,STARS[i].dec_deg,now,alt,az);if(alt<=0)continue;double angle=(az+rotation)*M_PI/180.0,r=(90.0-alt)/90.0*100.0;int x=191+(int)(r*sin(angle)),y=122-(int)(r*cos(angle));int size=STARS[i].mag<0?8:STARS[i].mag<0.6?6:4;lv_obj_t*d=lv_obj_create(g_sky_map_box);lv_obj_set_pos(d,x-size/2,y-size/2);lv_obj_set_size(d,size,size);lv_obj_set_style_radius(d,LV_RADIUS_CIRCLE,0);lv_obj_set_style_bg_color(d,lv_color_hex(STARS[i].mag<0?ACCENT:0xFFFFFF),0);lv_obj_set_style_shadow_color(d,lv_color_hex(COL_CYAN),0);lv_obj_set_style_shadow_width(d,6,0);lv_obj_set_style_border_width(d,0,0);if(STARS[i].mag<0.1f)label(g_sky_map_box,STARS[i].name,&lv_font_montserrat_12,COL_MUTED,LV_ALIGN_TOP_LEFT,x+5,y-8);}
  double ra,dec,alt,az;moon_equatorial(now,ra,dec);equatorial_to_horizontal(ra,dec,now,alt,az);if(alt>0){double angle=(az+rotation)*M_PI/180.0,r=(90-alt)/90*100;lv_obj_t*m=lv_obj_create(g_sky_map_box);lv_obj_set_pos(m,185+(int)(r*sin(angle)),116-(int)(r*cos(angle)));lv_obj_set_size(m,12,12);lv_obj_set_style_radius(m,LV_RADIUS_CIRCLE,0);lv_obj_set_style_bg_color(m,lv_color_hex(ACCENT),0);lv_obj_set_style_border_width(m,0,0);}
}
static uint8_t astro_brightness(){return g_dim_mode?48:DEFAULT_BRIGHTNESS;}
static void red_torch_close(lv_event_t*){if(g_red_overlay){lv_obj_del_async(g_red_overlay);g_red_overlay=nullptr;}fade_brightness(astro_brightness(),120);}
static void show_red_torch(){if(g_red_overlay)return;g_red_overlay=lv_obj_create(lv_layer_top());lv_obj_set_size(g_red_overlay,LCD_W,LCD_H);lv_obj_align(g_red_overlay,LV_ALIGN_CENTER,0,0);lv_obj_set_style_bg_color(g_red_overlay,lv_color_hex(0x5A0000),0);lv_obj_set_style_bg_grad_color(g_red_overlay,lv_color_hex(0x120000),0);lv_obj_set_style_bg_grad_dir(g_red_overlay,LV_GRAD_DIR_VER,0);lv_obj_set_style_bg_opa(g_red_overlay,LV_OPA_COVER,0);lv_obj_set_style_border_width(g_red_overlay,0,0);label(g_red_overlay,"RED OBSERVING LIGHT",&lv_font_montserrat_22,0xFF6B6B,LV_ALIGN_TOP_MID,0,52);label(g_red_overlay,"STEADY / LOW GLARE",&lv_font_montserrat_14,0xFF9090,LV_ALIGN_CENTER,0,-20);button(g_red_overlay,"CLOSE",70,390,270,54,red_torch_close,nullptr);fade_brightness(85,120);}

static bool copy_text_file(const String&src,const String&dst){File in=SD_MMC.open(src.c_str(),FILE_READ);if(!in)return false;String tmp=dst+".tmp";SD_MMC.remove(tmp.c_str());File out=SD_MMC.open(tmp.c_str(),FILE_WRITE);if(!out){in.close();return false;}char line[180];while(in.available()){size_t n=in.readBytesUntil('\n',line,sizeof(line)-1);line[n]=0;out.println(line);}in.close();out.flush();out.close();SD_MMC.remove(dst.c_str());return SD_MMC.rename(tmp.c_str(),dst.c_str());}
static void observation_done(lv_event_t*e){lv_event_code_t code=lv_event_get_code(e);if(code!=LV_EVENT_READY&&code!=LV_EVENT_CANCEL)return;if(code==LV_EVENT_READY&&g_obs_area&&g_sd_ok){char text[192];snprintf(text,sizeof(text),"%s",lv_textarea_get_text(g_obs_area));for(char*p=text;*p;p++)if(*p==','||*p=='\n'||*p=='\r')*p=' ';if(text[0]){char*part[6]={text,(char*)"",(char*)"",(char*)"",(char*)"",(char*)""};char*p=text;for(int i=1;i<6;i++){char*bar=strchr(p,'|');if(!bar)break;*bar=0;p=bar+1;part[i]=p;}String path=String(OS_FOLDER)+"/OBSERVATIONS/observations.csv";bool fresh=!SD_MMC.exists(path.c_str());File f=SD_MMC.open(path.c_str(),FILE_APPEND);if(f){if(fresh)f.println("unix,site,latitude,longitude,object,sky,equipment,visibility,rating,notes");f.printf("%lu,%s,%.5f,%.5f,%s,%s,%s,%s,%s,%s\n",(unsigned long)time(nullptr),g_location_set?SITES[g_site_index].name:"MANUAL/UNSET",g_lat,g_lon,part[0],part[1],part[2],part[3],part[4],part[5]);f.flush();f.close();}}}g_obs_area=nullptr;if(g_editor){lv_obj_del_async(g_editor);g_editor=nullptr;}if(g_overlay_status)lv_label_set_text(g_overlay_status,code==LV_EVENT_READY?"OBSERVATION SAVED":"ENTRY CANCELLED");}
static void open_observation_editor(){if(g_editor)return;g_editor=lv_obj_create(lv_layer_top());lv_obj_set_size(g_editor,LCD_W,LCD_H);lv_obj_align(g_editor,LV_ALIGN_CENTER,0,0);lv_obj_set_style_bg_color(g_editor,lv_color_hex(0x050709),0);lv_obj_set_style_bg_opa(g_editor,LV_OPA_COVER,0);lv_obj_set_style_border_width(g_editor,0,0);label(g_editor,"OBJECT | SKY | GEAR | VIS | 1-5 | NOTES",&lv_font_montserrat_14,ACCENT,LV_ALIGN_TOP_MID,0,12);g_obs_area=lv_textarea_create(g_editor);lv_obj_set_pos(g_obs_area,12,46);lv_obj_set_size(g_obs_area,386,88);lv_textarea_set_one_line(g_obs_area,true);lv_textarea_set_max_length(g_obs_area,180);lv_textarea_set_placeholder_text(g_obs_area,"Saturn | clear | 8in scope | good | 5 | rings");lv_obj_t*kb=lv_keyboard_create(g_editor);lv_obj_set_pos(kb,0,145);lv_obj_set_size(kb,LCD_W,357);lv_keyboard_set_textarea(kb,g_obs_area);lv_obj_add_event_cb(kb,observation_done,LV_EVENT_READY,nullptr);lv_obj_add_event_cb(kb,observation_done,LV_EVENT_CANCEL,nullptr);}
static void astro_update(){
  if(!g_overlay_value||g_feature_id<0)return;char out[900]={0};time_t now=time(nullptr);bool rtc_ok=now>=1700000000;
  if(g_feature_id==0){double alt=0,az=0;if(g_location_set)equatorial_to_horizontal(STARS[g_item_index%(int)(sizeof(STARS)/sizeof(STARS[0]))].ra_h,STARS[g_item_index%(int)(sizeof(STARS)/sizeof(STARS[0]))].dec_deg,now,alt,az);float roll=atan2f(g_ay,g_az)*180/M_PI,pitch=atan2f(-g_ax,sqrtf(g_ay*g_ay+g_az*g_az))*180/M_PI;snprintf(out,sizeof(out),"RELATIVE AZIMUTH  %.1f deg\nROLL / PITCH     %.1f / %.1f deg\nTARGET           %s\nTARGET ALT / AZ  %.1f / %.1f deg\nTURN             %+.1f deg\n\nFace true north and tap SET NORTH. The QMI8658 heading is gyro-relative and drifts; no magnetometer or GPS is assumed.",g_sky_heading,roll,pitch,STARS[g_item_index%(int)(sizeof(STARS)/sizeof(STARS[0]))].name,alt,az,fmod(az-g_sky_heading+540.0,360.0)-180.0);}
  else if(g_feature_id==1){const Star&s=STARS[g_item_index%(int)(sizeof(STARS)/sizeof(STARS[0]))];double alt=0,az=0;if(g_location_set&&rtc_ok)equatorial_to_horizontal(s.ra_h,s.dec_deg,now,alt,az);snprintf(out,sizeof(out),"%s%s\nCONSTELLATION  %s\nMAGNITUDE      %.2f\nRA / DEC       %.4f h / %.4f deg\nALT / AZ       %s %.1f / %.1f deg\nVISIBILITY     %s\nNOTE           %s\n\nStored coordinates; calculated alt/az.",(g_star_favs&(1UL<<g_item_index))?"* FAVOURITE  ":"",s.name,s.constellation,s.mag,s.ra_h,s.dec_deg,g_location_set&&rtc_ok?"":"UNAVAILABLE",alt,az,g_location_set&&rtc_ok?(alt>10?"ABOVE 10 DEG":"BELOW 10 DEG"):"SET LOCATION / CLOCK",STAR_NOTES[g_item_index]);}
  else if(g_feature_id==2){const Constellation&c=CONSTELLATIONS[g_item_index%(int)(sizeof(CONSTELLATIONS)/sizeof(CONSTELLATIONS[0]))];int anchor[]={5,11,4,13,4};double alt=0,az=0;if(g_location_set)equatorial_to_horizontal(STARS[anchor[g_item_index%5]].ra_h,STARS[anchor[g_item_index%5]].dec_deg,now,alt,az);snprintf(out,sizeof(out),"%s  ALT %.0f / AZ %.0f\n%s\nGuide: %s\nDiagram is a north-up recognition aid, not to scale.",c.name,alt,az,c.stars,c.guide);constellation_draw();}
  else if(g_feature_id==3){int planet_index=g_item_index%(int)(sizeof(PLANETS)/sizeof(PLANETS[0]));const Planet&p=PLANETS[planet_index];double ra=0,dec=0,alt=0,az=0;planet_approx(planet_index,now,ra,dec);if(g_location_set)equatorial_to_horizontal(ra,dec,now,alt,az);snprintf(out,sizeof(out),"%s  %s\nORBIT / ROTATION %.1f d / %.1f h\nDISTANCE EST     %.2f AU\nTYPICAL MAG      %.1f\nRADIUS / GRAVITY %.0f km / %.2f m/s2\nAPPROX RA / DEC  %.2f h / %.1f deg\nALT / AZ         %.1f / %.1f deg\nVISIBILITY       %s\n\nCircular-orbit estimate; confirm with current ephemeris.",p.name,p.fact,p.period_days,p.rotation_h,planet_distance_au(planet_index,now),p.typical_mag,p.radius_km,p.gravity,ra,dec,alt,az,alt>10?"ABOVE 10 DEG":"LOW / BELOW HORIZON");}
  else if(g_feature_id==4){double age=moon_age_days(now),illum=(1.0-cos(2*M_PI*age/29.53058867))*50.0,next_full=fmod(14.7653-age+29.53058867,29.53058867),next_new=fmod(29.53058867-age,29.53058867),ra,dec,alt=0,az=0;moon_equatorial(now,ra,dec);if(g_location_set)equatorial_to_horizontal(ra,dec,now,alt,az);char rise[8],set[8];moon_rise_set(now,rise,sizeof(rise),set,sizeof(set));snprintf(out,sizeof(out),"PHASE / TREND  %s / %s\nAGE / LIGHT    %.2f d / %.1f%%\nALT / AZ       %.1f / %.1f deg\nMOONRISE / SET %s / %s\nNEXT FULL / NEW %.1f / %.1f days\n\nMean-cycle phase and simplified lunar position/rise-set. Confirm precise events externally.",moon_phase_name(age),age<14.7653?"WAXING":"WANING",age,illum,alt,az,rise,set,next_full,next_new);}
  else if(g_feature_id==5){char rise[8]="--",set[8]="--",civil_r[8]="--",civil_s[8]="--",naut_r[8]="--",naut_s[8]="--",astro_r[8]="--",astro_s[8]="--";double a,b,day_h=0;bool ok=g_location_set&&sun_event(90.833,now,a,b);if(ok){format_hour(a,rise,sizeof(rise));format_hour(b,set,sizeof(set));day_h=b-a;if(day_h<0)day_h+=24;}bool c=g_location_set&&sun_event(96,now,a,b);if(c){format_hour(a,civil_r,sizeof(civil_r));format_hour(b,civil_s,sizeof(civil_s));}bool n=g_location_set&&sun_event(102,now,a,b);if(n){format_hour(a,naut_r,sizeof(naut_r));format_hour(b,naut_s,sizeof(naut_s));}bool d=g_location_set&&sun_event(108,now,a,b);if(d){format_hour(a,astro_r,sizeof(astro_r));format_hour(b,astro_s,sizeof(astro_s));}double dec,eq;solar_terms(now,dec,eq);double noon=(720-4*g_lon-eq+g_tz*60)/60;while(noon<0)noon+=24;while(noon>=24)noon-=24;char noon_s[8];format_hour(noon,noon_s,sizeof(noon_s));snprintf(out,sizeof(out),"RISE / NOON / SET %s / %s / %s\nCIVIL       %s / %s\nNAUTICAL    %s / %s\nASTRONOMICAL %s / %s\nDAY / NIGHT %.1f / %.1f h\nSOLAR DEC   %.2f deg\n\nManual site and fixed UTC offset; set DST manually.",rise,noon_s,set,civil_r,civil_s,naut_r,naut_s,astro_r,astro_s,day_h,24-day_h,dec);}
  else if(g_feature_id==6){double age=moon_age_days(now),target[]={0,7.3826,14.7653,22.1479};const char*names[]={"New moon","First quarter","Full moon","Last quarter"};int soon=0;double best=99;for(int i=0;i<4;i++){double delta=fmod(target[i]-age+29.53058867,29.53058867);if(delta<best){best=delta;soon=i;}}struct tm t={};gmtime_r(&now,&t);int doy=t.tm_yday+1;struct Shower{const char*n;int d;};static const Shower showers[]={{"Quadrantids",4},{"Lyrids",112},{"Eta Aquariids",126},{"Perseids",224},{"Orionids",294},{"Leonids",321},{"Geminids",348}};int si=0,dd=999;for(int i=0;i<7;i++){int delta=showers[i].d-doy;if(delta<0)delta+=365;if(delta<dd){dd=delta;si=i;}}int seasons[]={79,172,265,355};const char*season_names[]={"March equinox","June solstice","September equinox","December solstice"};int se=0,sd=999;for(int i=0;i<4;i++){int delta=seasons[i]-doy;if(delta<0)delta+=365;if(delta<sd){sd=delta;se=i;}}bool imported=SD_MMC.exists((String(OS_FOLDER)+"/EVENTS/events.csv").c_str());snprintf(out,sizeof(out),"NEXT PHASE  %s / %.1f d\nNEXT SHOWER %s / ~%d d\nNEXT SEASON  %s / ~%d d\nREMINDER     %s\nIMPORTED     %s\n\nImported events may include eclipses, conjunctions and visibility windows. Offline dates are approximate; confirm locally.",names[soon],best,showers[si].n,dd,season_names[se],sd,g_event_reminder?"ON":"OFF",imported?"events.csv READY":"NONE");}
  else if(g_feature_id==7){char name[40]="--";double epoch,inc,raan,ecc,argp,ma,mm;if(!g_location_set)snprintf(out,sizeof(out),"SET A MANUAL LOCATION FIRST.\n\nThen place a current three-line element set at:\nWATCH/AstroOS/SATELLITES/tle.txt");else if(!parse_tle(name,sizeof(name),epoch,inc,raan,ecc,argp,ma,mm))snprintf(out,sizeof(out),"NO VALID TLE LOADED\n\nExpected file:\nWATCH/AstroOS/SATELLITES/tle.txt\n\nLine 1: object name\nLine 2: TLE line 1\nLine 3: TLE line 2");else{double alt,az;tle_altaz(now,epoch,inc,raan,ecc,argp,ma,mm,alt,az);time_t r=0,p=0,s=0;float peak=-90;bool pass=next_sat_pass(now,epoch,inc,raan,ecc,argp,ma,mm,r,p,s,peak);char rr[8]="--",pp[8]="--",ss[8]="--",peak_s[16]="--";if(pass){struct tm q;time_t rt=r+g_tz*3600,pt=p+g_tz*3600,st=s+g_tz*3600;gmtime_r(&rt,&q);strftime(rr,sizeof(rr),"%H:%M",&q);gmtime_r(&pt,&q);strftime(pp,sizeof(pp),"%H:%M",&q);gmtime_r(&st,&q);strftime(ss,sizeof(ss),"%H:%M",&q);snprintf(peak_s,sizeof(peak_s),"%.1f deg",peak);}snprintf(out,sizeof(out),"%s\nTLE AGE       %.1f days\nNOW ALT / AZ  %.1f / %.1f deg\nNEXT >10 DEG  %s - %s - %s\nPEAK ALT      %s\n\nApproximate two-body propagation, not SGP4. Old TLEs rapidly lose accuracy; confirm passes before relying on them.",name,fabs((double)now-epoch)/86400.0,alt,az,rr,pp,ss,peak_s);}}
  else if(g_feature_id==8){const Body&b=BODIES[g_orbit_body];double r=b.radius_km+g_orbit_alt_km,v=sqrt(b.mu_km3_s2/r),esc=sqrt(2*b.mu_km3_s2/r),period=2*M_PI*sqrt(r*r*r/b.mu_km3_s2)/60.0,grav=b.mu_km3_s2/(r*r)*1000.0,force=grav*1000.0;snprintf(out,sizeof(out),"BODY / ALT     %s / %.0f km\nCIRC VELOCITY  %.3f km/s\nESC VELOCITY   %.3f km/s\nPERIOD         %.1f min\nGRAVITY        %.3f m/s2\nFORCE @1000kg  %.0f N\nBODY MASS      %.3e kg\n\nKepler 3: period squared scales with orbit radius cubed. Ideal two-body circular model.",b.name,g_orbit_alt_km,v,esc,period,grav,force,b.mass_kg);}
  else if(g_feature_id==9){snprintf(out,sizeof(out),"OBSERVATION LOG\n\nStructured fields: object, date/time, manual location, sky, equipment, visibility, rating and notes.\n\nEnter six fields separated by |. Date/time and site are attached automatically.\n\nWATCH/AstroOS/OBSERVATIONS/observations.csv");}
  else if(g_feature_id==10){const char*field=g_location_field==0?"LATITUDE":g_location_field==1?"LONGITUDE":"UTC OFFSET";snprintf(out,sizeof(out),"MANUAL SITE   %s\nLATITUDE      %.4f deg\nLONGITUDE     %.4f deg\nUTC OFFSET    %+d h\nEDIT FIELD    %s\nSTATUS        %s\n\nNo GPS is used. Select a preset, then adjust values. Set DST manually when applicable.",SITES[g_site_index].name,g_lat,g_lon,g_tz,field,g_location_set?"SAVED":"NOT SET");}
  else if(g_feature_id==11){snprintf(out,sizeof(out),"EXPORTS\n\nObservation CSV and manual location can be copied atomically to WATCH/FUSION_SHARED. No GPS-derived fields are produced.");}
  else if(g_feature_id==12){sky_map_draw();snprintf(out,sizeof(out),"%s // %s\nBRIGHT STARS + MOON / MANUAL SITE + CLOCK",g_sky_map_track_up?"RELATIVE HEADING":"NORTH UP",g_location_set?SITES[g_site_index].name:"LOCATION REQUIRED");}
  else if(g_feature_id==13){
    if(!g_location_set||!rtc_ok)snprintf(out,sizeof(out),"PLANNER UNAVAILABLE\n\nSet a manual observing location and a valid watch clock. No GPS or network lookup is used.");
    else{
      int best[5]={-1,-1,-1,-1,-1};double score[5]={-999,-999,-999,-999,-999},alts[5]={0},azs[5]={0};
      for(int i=0;i<(int)(sizeof(STARS)/sizeof(STARS[0]));i++){double alt,az;equatorial_to_horizontal(STARS[i].ra_h,STARS[i].dec_deg,now,alt,az);if(alt<10)continue;double s=alt-STARS[i].mag*8.0;for(int k=0;k<5;k++)if(s>score[k]){for(int j=4;j>k;j--){score[j]=score[j-1];best[j]=best[j-1];alts[j]=alts[j-1];azs[j]=azs[j-1];}score[k]=s;best[k]=i;alts[k]=alt;azs[k]=az;break;}}
      size_t used=snprintf(out,sizeof(out),"TONIGHT'S BEST BRIGHT TARGETS\n\n");int count=0;for(int k=0;k<5&&best[k]>=0;k++){const char*dir=azs[k]<22.5||azs[k]>=337.5?"N":azs[k]<67.5?"NE":azs[k]<112.5?"E":azs[k]<157.5?"SE":azs[k]<202.5?"S":azs[k]<247.5?"SW":azs[k]<292.5?"W":"NW";used+=snprintf(out+used,sizeof(out)-used,"%d  %-11s  %s  %.0f deg high\n",k+1,STARS[best[k]].name,dir,alts[k]);count++;}
      double age=moon_age_days(now),illum=(1.0-cos(2*M_PI*age/29.53058867))*50.0;snprintf(out+used,sizeof(out)-used,"\nMOONLIGHT  %.0f%% / %s\n\nRanking favours altitude and brightness. It is a compact built-in catalogue, not a complete observing forecast.%s",illum,moon_phase_name(age),count?"":"\nNo bright targets are currently above 10 deg.");
    }
  }
  else if(g_feature_id==14){uint32_t elapsed=g_dark_timer_running?(millis()-g_dark_timer_started):0;if(g_dark_timer_running&&elapsed>=1200000UL){g_dark_timer_running=false;g_dark_timer_complete=true;elapsed=1200000UL;}uint32_t left=g_dark_timer_complete?0:elapsed>=1200000UL?0:1200000UL-elapsed;snprintf(out,sizeof(out),"NIGHT-VISION CONTROL\n\nRED TORCH       steady low-glare screen\nDISPLAY         %s\nDARK TIMER      %s\nTIME REMAINING  %02lu:%02lu\n\nThe 20-minute timer is only a reminder; it cannot measure your eyes' actual adaptation.",g_dim_mode?"DIM":"NORMAL",g_dark_timer_complete?"COMPLETE":g_dark_timer_running?"RUNNING":"NOT STARTED",(unsigned long)(left/60000UL),(unsigned long)((left/1000UL)%60UL));}
  lv_label_set_text(g_overlay_value,out);
}

static void astro_control(lv_event_t*e){
  int action=(int)(intptr_t)lv_event_get_user_data(e);int star_n=sizeof(STARS)/sizeof(STARS[0]);
  if(action==-1||action==1){int n=g_feature_id==1?star_n:g_feature_id==2?(int)(sizeof(CONSTELLATIONS)/sizeof(CONSTELLATIONS[0])):g_feature_id==3?(int)(sizeof(PLANETS)/sizeof(PLANETS[0])):star_n;g_item_index=(g_item_index+action+n)%n;}
  else if(action==10){g_star_favs^=(1UL<<g_item_index);Preferences p;p.begin("astro",false);p.putULong("favs",g_star_favs);p.end();}
  else if(action==20)g_sky_heading=0;
  else if(action==21)g_sky_heading=norm_deg(g_sky_heading-5);
  else if(action==22)g_sky_heading=norm_deg(g_sky_heading+5);
  else if(action==30){g_site_index=(g_site_index+1)%(sizeof(SITES)/sizeof(SITES[0]));g_lat=SITES[g_site_index].lat;g_lon=SITES[g_site_index].lon;g_tz=SITES[g_site_index].tz;g_location_set=true;save_location();}
  else if(action==31)g_location_field=(g_location_field+1)%3;
  else if(action==32||action==33){float step=action==32?-1:1;if(g_location_field==0)g_lat=max(-89.0f,min(89.0f,g_lat+step));else if(g_location_field==1)g_lon=max(-180.0f,min(180.0f,g_lon+step*5));else g_tz=max(-12,min(14,g_tz+(int)step));g_location_set=true;save_location();}
  else if(action==40)g_orbit_body=(g_orbit_body-1+(int)(sizeof(BODIES)/sizeof(BODIES[0])))%(sizeof(BODIES)/sizeof(BODIES[0]));
  else if(action==41)g_orbit_body=(g_orbit_body+1)%(sizeof(BODIES)/sizeof(BODIES[0]));
  else if(action==42)g_orbit_alt_km=max(0.0f,g_orbit_alt_km-100);
  else if(action==43)g_orbit_alt_km=min(50000.0f,g_orbit_alt_km+100);
  else if(action==60||action==61){bool ok=false;if(action==60)ok=copy_text_file(String(OS_FOLDER)+"/OBSERVATIONS/observations.csv","/WATCH/FUSION_SHARED/EXPORTS/astro_observations.csv");else{String body=String("schema,source,site,latitude,longitude,utc_offset\n2,AstroOS,")+SITES[g_site_index].name+","+String(g_lat)+","+String(g_lon)+","+String(g_tz)+"\n";ok=atomic_write("/WATCH/FUSION_SHARED/LOCATIONS/astro_location.csv",body);}if(g_overlay_status)lv_label_set_text(g_overlay_status,ok?"EXPORT COMPLETE":"NO DATA / STORAGE ERROR");}
  else if(action==70)open_observation_editor();
  else if(action==80){g_event_reminder=!g_event_reminder;Preferences p;p.begin("astro",false);p.putBool("reminder",g_event_reminder);p.end();}
  else if(action==90){g_dark_timer_started=millis();g_dark_timer_running=true;g_dark_timer_complete=false;}
  else if(action==91){g_dim_mode=!g_dim_mode;Preferences p;p.begin("astro",false);p.putBool("dim",g_dim_mode);p.end();fade_brightness(astro_brightness(),160);}
  else if(action==92)show_red_torch();
  else if(action==93){g_sky_map_track_up=!g_sky_map_track_up;Preferences p;p.begin("astro",false);p.putBool("trackup",g_sky_map_track_up);p.end();sky_map_draw();}
  astro_update();
}

static void feature_click(lv_event_t *e){
  if(millis()-g_last_swipe_ms<CLICK_AFTER_SWIPE_MS)return;
  int i=(int)(uintptr_t)lv_event_get_user_data(e);if(i<0||i>=(int)FEATURE_COUNT)return;
  if(g_overlay)lv_obj_del(g_overlay);g_feature_id=i;g_item_index=0;
  g_overlay=lv_obj_create(lv_layer_top());lv_obj_set_size(g_overlay,LCD_W,LCD_H);lv_obj_align(g_overlay,LV_ALIGN_CENTER,0,0);lv_obj_clear_flag(g_overlay,LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(g_overlay,lv_color_hex(0x060914),0);lv_obj_set_style_bg_grad_color(g_overlay,lv_color_hex(0x01030A),0);lv_obj_set_style_bg_grad_dir(g_overlay,LV_GRAD_DIR_VER,0);lv_obj_set_style_bg_opa(g_overlay,LV_OPA_COVER,0);lv_obj_set_style_border_width(g_overlay,0,0);lv_obj_set_style_radius(g_overlay,0,0);
  label(g_overlay,FEATURES[i].name,&lv_font_montserrat_22,ACCENT,LV_ALIGN_TOP_LEFT,16,15);button(g_overlay,"X",354,9,40,38,close_overlay,nullptr);
  lv_obj_t*panel=lv_obj_create(g_overlay);lv_obj_t*text_parent=panel;
  if(i==2){g_constellation_box=panel;lv_obj_set_pos(panel,14,56);lv_obj_set_size(panel,382,145);text_parent=lv_obj_create(g_overlay);lv_obj_set_pos(text_parent,14,208);lv_obj_set_size(text_parent,382,118);}
  else if(i==12){g_sky_map_box=panel;lv_obj_set_pos(panel,14,56);lv_obj_set_size(panel,382,250);text_parent=lv_obj_create(g_overlay);lv_obj_set_pos(text_parent,14,313);lv_obj_set_size(text_parent,382,72);}
  else{lv_obj_set_pos(panel,14,56);lv_obj_set_size(panel,382,270);}
  lv_obj_t*styled[2]={panel,text_parent};for(int n=0;n<2;n++){if(n==1&&text_parent==panel)continue;lv_obj_clear_flag(styled[n],LV_OBJ_FLAG_SCROLLABLE);lv_obj_set_style_bg_color(styled[n],lv_color_hex(COL_CARD),0);lv_obj_set_style_bg_grad_color(styled[n],lv_color_hex(0x070A16),0);lv_obj_set_style_bg_grad_dir(styled[n],LV_GRAD_DIR_VER,0);lv_obj_set_style_border_color(styled[n],lv_color_hex(COL_BORDER),0);lv_obj_set_style_border_width(styled[n],1,0);lv_obj_set_style_radius(styled[n],16,0);}
  g_overlay_value=label(text_parent,"",&lv_font_montserrat_14,COL_TEXT,LV_ALIGN_TOP_LEFT,10,9);lv_label_set_long_mode(g_overlay_value,LV_LABEL_LONG_WRAP);lv_obj_set_width(g_overlay_value,354);
  if(i==0){button(g_overlay,"SET NORTH",14,338,181,44,astro_control,(void*)20);button(g_overlay,"TARGET >",215,338,181,44,astro_control,(void*)1);button(g_overlay,"-5",14,390,181,42,astro_control,(void*)21);button(g_overlay,"+5",215,390,181,42,astro_control,(void*)22);}
  else if(i==1){button(g_overlay,"<",14,338,112,44,astro_control,(void*)-1);button(g_overlay,"FAV",149,338,112,44,astro_control,(void*)10);button(g_overlay,">",284,338,112,44,astro_control,(void*)1);}
  else if(i==2||i==3){button(g_overlay,"< PREV",14,350,181,48,astro_control,(void*)-1);button(g_overlay,"NEXT >",215,350,181,48,astro_control,(void*)1);}
  else if(i==6)button(g_overlay,"TOGGLE REMINDER",76,350,230,48,astro_control,(void*)80);
  else if(i==7)button(g_overlay,"REFRESH TLE",76,350,230,48,astro_control,(void*)50);
  else if(i==8){button(g_overlay,"< BODY",14,338,181,44,astro_control,(void*)40);button(g_overlay,"BODY >",215,338,181,44,astro_control,(void*)41);button(g_overlay,"ALT -100",14,390,181,42,astro_control,(void*)42);button(g_overlay,"ALT +100",215,390,181,42,astro_control,(void*)43);}
  else if(i==9)button(g_overlay,"ADD OBSERVATION",62,350,258,52,astro_control,(void*)70);
  else if(i==10){button(g_overlay,"NEXT PRESET",14,338,181,44,astro_control,(void*)30);button(g_overlay,"NEXT FIELD",215,338,181,44,astro_control,(void*)31);button(g_overlay,"-",14,390,181,42,astro_control,(void*)32);button(g_overlay,"+",215,390,181,42,astro_control,(void*)33);}
  else if(i==11){button(g_overlay,"EXPORT OBSERVATIONS",14,338,382,44,astro_control,(void*)60);button(g_overlay,"EXPORT LOCATION",14,390,382,42,astro_control,(void*)61);}
  else if(i==12){button(g_overlay,"NORTH / TRACK UP",14,393,382,44,astro_control,(void*)93);}
  else if(i==13)button(g_overlay,"REFRESH PLAN",76,350,230,48,astro_control,(void*)50);
  else if(i==14){button(g_overlay,"20 MIN TIMER",14,338,181,44,astro_control,(void*)90);button(g_overlay,"DIM MODE",215,338,181,44,astro_control,(void*)91);button(g_overlay,"RED TORCH",14,390,382,42,astro_control,(void*)92);}
  g_overlay_status=label(g_overlay,"",&lv_font_montserrat_12,COL_MUTED,LV_ALIGN_BOTTOM_MID,0,-8);astro_update();
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
  lv_obj_set_style_bg_color(s,lv_color_hex(0x07061A),0);lv_obj_set_style_bg_grad_color(s,lv_color_hex(COL_BG),0);lv_obj_set_style_bg_grad_dir(s,LV_GRAD_DIR_VER,0);lv_obj_set_style_bg_opa(s,LV_OPA_COVER,0);
  lv_obj_clear_flag(s,LV_OBJ_FLAG_SCROLLABLE);
}
static void dashboard_tools_click(lv_event_t*){set_screen(1);}

static void build_dashboard(){
  lv_obj_t *s=lv_obj_create(nullptr);g_screens[0]=s;init_screen(s);
  const int sx[]={91,132,174,238,271,360,44,330};const int sy[]={18,67,34,73,22,61,83,12};
  for(int i=0;i<8;i++){lv_obj_t*d=lv_obj_create(s);lv_obj_set_pos(d,sx[i],sy[i]);lv_obj_set_size(d,i%3==0?3:2,i%3==0?3:2);lv_obj_set_style_radius(d,LV_RADIUS_CIRCLE,0);lv_obj_set_style_bg_color(d,lv_color_hex(i%3==0?ACCENT:COL_CYAN),0);lv_obj_set_style_bg_opa(d,i%3==0?LV_OPA_70:LV_OPA_40,0);lv_obj_set_style_border_width(d,0,0);}
  label(s,"ASTRO",&lv_font_montserrat_28,COL_TEXT,LV_ALIGN_TOP_LEFT,18,12);
  label(s,"OBSERVATORY // OFFLINE",&lv_font_montserrat_12,ACCENT,LV_ALIGN_TOP_LEFT,20,51);
  g_clock=label(s,"--:--",&lv_font_montserrat_28,COL_TEXT,LV_ALIGN_TOP_RIGHT,-18,13);
  g_battery=label(s,"BAT --%",&lv_font_montserrat_12,COL_MUTED,LV_ALIGN_TOP_RIGHT,-20,53);
  lv_obj_t *card=lv_obj_create(s);lv_obj_set_pos(card,14,91);lv_obj_set_size(card,382,200);
  lv_obj_set_style_bg_color(card,lv_color_hex(COL_PANEL),0);lv_obj_set_style_bg_grad_color(card,lv_color_hex(0x070914),0);lv_obj_set_style_bg_grad_dir(card,LV_GRAD_DIR_HOR,0);lv_obj_set_style_border_color(card,lv_color_hex(COL_BORDER),0);lv_obj_set_style_border_width(card,1,0);lv_obj_set_style_radius(card,24,0);lv_obj_set_style_shadow_color(card,lv_color_hex(COL_NEBULA),0);lv_obj_set_style_shadow_width(card,14,0);lv_obj_set_style_shadow_opa(card,LV_OPA_30,0);
  label(card,"TONIGHT'S SKY",&lv_font_montserrat_12,ACCENT,LV_ALIGN_TOP_LEFT,16,14);
  g_sky_summary=label(card,"SET MANUAL LOCATION",&lv_font_montserrat_22,COL_TEXT,LV_ALIGN_TOP_LEFT,16,41);
  g_sensor=label(card,"MOON data available when the watch clock is valid",&lv_font_montserrat_14,COL_MUTED,LV_ALIGN_TOP_LEFT,16,84);lv_label_set_long_mode(g_sensor,LV_LABEL_LONG_WRAP);lv_obj_set_width(g_sensor,254);
  g_dash_moon_disc=lv_obj_create(card);lv_obj_set_pos(g_dash_moon_disc,303,54);lv_obj_set_size(g_dash_moon_disc,54,54);lv_obj_set_style_radius(g_dash_moon_disc,LV_RADIUS_CIRCLE,0);lv_obj_set_style_bg_color(g_dash_moon_disc,lv_color_hex(0xFFF0B5),0);lv_obj_set_style_shadow_color(g_dash_moon_disc,lv_color_hex(ACCENT),0);lv_obj_set_style_shadow_width(g_dash_moon_disc,14,0);lv_obj_set_style_shadow_opa(g_dash_moon_disc,LV_OPA_50,0);lv_obj_set_style_border_width(g_dash_moon_disc,0,0);
  g_dash_moon_shadow=lv_obj_create(card);lv_obj_set_pos(g_dash_moon_shadow,303,54);lv_obj_set_size(g_dash_moon_shadow,54,54);lv_obj_set_style_radius(g_dash_moon_shadow,LV_RADIUS_CIRCLE,0);lv_obj_set_style_bg_color(g_dash_moon_shadow,lv_color_hex(COL_PANEL),0);lv_obj_set_style_border_width(g_dash_moon_shadow,0,0);
  label(card,"MANUAL SITE / NO GPS",&lv_font_montserrat_12,COL_MUTED,LV_ALIGN_BOTTOM_LEFT,16,-14);
  button(s,"SKY MAP",14,307,122,48,feature_click,(void*)12);button(s,"MOON",144,307,122,48,feature_click,(void*)4);button(s,"PLANETS",274,307,122,48,feature_click,(void*)3);
  button(s,"PLANNER",14,365,122,48,feature_click,(void*)13);button(s,"NIGHT",144,365,122,48,feature_click,(void*)14);button(s,"ALL TOOLS",274,365,122,48,dashboard_tools_click,nullptr);
  label(s,"Swipe left for the observatory",&lv_font_montserrat_12,COL_MUTED,LV_ALIGN_BOTTOM_MID,0,-47);
  label(s,"Hold BOOT 1.5 s for LauncherOS",&lv_font_montserrat_12,0x59636D,LV_ALIGN_BOTTOM_MID,0,-19);
}

static void build_tools(){
  lv_obj_t *s=lv_obj_create(nullptr); g_screens[1]=s; init_screen(s);
  label(s,"OBSERVATORY",&lv_font_montserrat_22,COL_TEXT,LV_ALIGN_TOP_LEFT,20,17);
  char count[24];snprintf(count,sizeof(count),"%u OFFLINE TOOLS",(unsigned)FEATURE_COUNT);label(s,count,&lv_font_montserrat_12,ACCENT,LV_ALIGN_TOP_RIGHT,-20,24);
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
  save_clock();
  WiFi.disconnect(true,true); WiFi.mode(WIFI_OFF); btStop();
  Preferences p; p.begin("fusionenv",false); p.putUChar("screen",g_screen); p.end();
}

static void return_to_launcher(){
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
  lv_obj_invalidate(lv_scr_act()); pump(); fade_brightness(astro_brightness(),200);
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

static void astro_imu_tick(){uint32_t now=millis();if(!g_last_imu_ms)g_last_imu_ms=now;if(now-g_last_imu_ms<20)return;float dt=(now-g_last_imu_ms)/1000.0f;g_last_imu_ms=now;if(g_has_imu&&g_imu.getDataReady()){g_imu.getAccelerometer(g_ax,g_ay,g_az);g_imu.getGyroscope(g_gx,g_gy,g_gz);g_sky_heading=norm_deg(g_sky_heading+g_gz*dt);}}

static void update_live(){
  struct tm t; char clock_text[8]="--:--";
#if FUSION_ENV == 4
  time_t clock_now=time(nullptr);if(clock_now>=1700000000){clock_now+=g_tz*3600;gmtime_r(&clock_now,&t);strftime(clock_text,sizeof(clock_text),"%H:%M",&t);}
#else
  if(getLocalTime(&t,5))strftime(clock_text,sizeof(clock_text),"%H:%M",&t);
#endif
  if(g_clock) lv_label_set_text(g_clock,clock_text);
  int pct=g_has_pmu?g_pmu.getBatteryPercent():-1; char bat[20]; snprintf(bat,sizeof(bat),pct>=0?"BAT %d%%":"BAT --%%",pct); if(g_battery) lv_label_set_text(g_battery,bat);
  if(g_sensor){
    char out[128];
#if FUSION_ENV == 2
    float mag=sqrtf(g_ax*g_ax+g_ay*g_ay+g_az*g_az);
    snprintf(out,sizeof(out),"IMU LIVE   acceleration %.2f g",mag);
#elif FUSION_ENV == 3
    float mag=sqrtf(g_ax*g_ax+g_ay*g_ay+g_az*g_az);
    snprintf(out,sizeof(out),"IMU %s   |a| %.3f g",g_has_imu?"CONNECTED":"OFF",mag);
#elif FUSION_ENV == 4
    time_t now=time(nullptr);double age=moon_age_days(now),illum=(1-cos(2*M_PI*age/29.53058867))*50.0,a=0,b=0;char sunset[8]="--";bool solar=g_location_set&&sun_event(90.833,now,a,b);if(solar)format_hour(b,sunset,sizeof(sunset));double next_full=fmod(14.7653-age+29.53058867,29.53058867);snprintf(out,sizeof(out),"MOON  %s / %.0f%%\nSUNSET %s   FULL IN %.1f d\nSITE   %s",moon_phase_name(age),illum,sunset,next_full,g_location_set?SITES[g_site_index].name:"SET MANUALLY");if(g_sky_summary){const char*state="SET MANUAL LOCATION";if(solar){time_t local=now+g_tz*3600;struct tm lt;gmtime_r(&local,&lt);double hour=lt.tm_hour+lt.tm_min/60.0;state=(hour>=a&&hour<b)?"DAYLIGHT":"OBSERVING WINDOW";}lv_label_set_text(g_sky_summary,state);}if(g_dash_moon_shadow){int shadow_x=age<14.7653?303+(int)(illum*0.44):303-(int)(illum*0.44);lv_obj_set_x(g_dash_moon_shadow,shadow_x);}
#else
    snprintf(out,sizeof(out),"LOCATION MODULE UNAVAILABLE");
#endif
    lv_label_set_text(g_sensor,out);
  }
  if(g_feature_id!=2&&g_feature_id!=7&&g_feature_id!=12)astro_update();
}

static void hardware_init(){
  Serial.begin(115200); pinMode(BOOT_BTN,INPUT_PULLUP); pinMode(TP_INT,INPUT_PULLUP); btStop();
  gfx->begin(); gfx->fillScreen(0); static_cast<Arduino_CO5300*>(gfx)->setBrightness(0);
  pinMode(IIC_SCL,OUTPUT); for(int i=0;i<9;i++){ digitalWrite(IIC_SCL,LOW); delayMicroseconds(5); digitalWrite(IIC_SCL,HIGH); delayMicroseconds(5); }
  Wire.begin(IIC_SDA,IIC_SCL); Wire.setClock(400000);
  pinMode(TP_RESET,OUTPUT); digitalWrite(TP_RESET,LOW); delay(50); digitalWrite(TP_RESET,HIGH); delay(200); FT3168->begin();
  if(g_pmu.begin(Wire,AXP2101_SLAVE_ADDRESS,IIC_SDA,IIC_SCL)){ g_has_pmu=true; g_pmu.disableTSPinMeasure(); g_pmu.enableBattVoltageMeasure(); }
  if(g_imu.begin(Wire,QMI8658_L_SLAVE_ADDRESS,IIC_SDA,IIC_SCL)){g_has_imu=true;g_imu.configAccelerometer(SensorQMI8658::ACC_RANGE_4G,SensorQMI8658::ACC_ODR_125Hz,SensorQMI8658::LPF_MODE_0);g_imu.enableAccelerometer();g_imu.configGyroscope(SensorQMI8658::GYR_RANGE_1024DPS,SensorQMI8658::GYR_ODR_224_2Hz,SensorQMI8658::LPF_MODE_0);g_imu.enableGyroscope();}
  lv_init(); size_t pixels=LCD_W*60; g_lvbuf=(lv_color_t*)heap_caps_malloc(pixels*sizeof(lv_color_t),MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT); if(!g_lvbuf) g_lvbuf=(lv_color_t*)heap_caps_malloc(pixels*sizeof(lv_color_t),MALLOC_CAP_8BIT);
  lv_disp_draw_buf_init(&g_draw_buf,g_lvbuf,nullptr,pixels); lv_disp_drv_init(&g_disp_drv); g_disp_drv.flush_cb=display_flush; g_disp_drv.draw_buf=&g_draw_buf; g_disp_drv.hor_res=LCD_W; g_disp_drv.ver_res=LCD_H; lv_disp_drv_register(&g_disp_drv);
  lv_indev_drv_init(&g_indev_drv); g_indev_drv.type=LV_INDEV_TYPE_POINTER; g_indev_drv.read_cb=touch_read; lv_indev_drv_register(&g_indev_drv);
}

void setup(){
  hardware_init(); restore_clock(); build_splash(); delay(350);
  SD_MMC.setPins(SDMMC_CLK,SDMMC_CMD,SDMMC_DATA); g_sd_ok=SD_MMC.begin("/sdcard",true); ensure_storage();
  {Preferences a;a.begin("astro",true);g_location_set=a.getBool("loc",false);g_lat=a.getFloat("lat",0);g_lon=a.getFloat("lon",0);g_tz=max(-12,min(14,a.getInt("tz",0)));g_star_favs=a.getULong("favs",0);g_event_reminder=a.getBool("reminder",false);g_dim_mode=a.getBool("dim",false);g_sky_map_track_up=a.getBool("trackup",false);a.end();for(int i=0;i<(int)(sizeof(SITES)/sizeof(SITES[0]));i++)if(fabs(g_lat-SITES[i].lat)<0.1&&fabs(g_lon-SITES[i].lon)<0.1)g_site_index=i;}
  build_dashboard(); build_tools(); build_system();
  Preferences p; p.begin("fusionenv",true); g_screen=min(2,(int)p.getUChar("screen",0)); p.end();
  lv_scr_load_anim(g_screens[g_screen],LV_SCR_LOAD_ANIM_FADE_ON,260,0,false);g_last_touch_ms=millis();update_live();fade_brightness(astro_brightness(),180);
}

void loop(){
  static uint32_t last_tick=millis(),last_live=0,last_sample=0,boot_press=0;
  uint32_t now=millis();
#if !defined(LV_TICK_CUSTOM) || (LV_TICK_CUSTOM == 0)
  lv_tick_inc(now-last_tick);
#endif
  last_tick=now;pump();astro_imu_tick();
  if(g_pending_swipe && !g_overlay){ int event=g_pending_swipe; g_pending_swipe=0; set_screen(g_screen+event); }
  if(now-last_live>=1000){ last_live=now; update_live(); }
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
