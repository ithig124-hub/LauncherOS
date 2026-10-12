// =============================================================================
// VoyagerOS V1.5.2 -- offline travel computer for ESP32-S3 Touch AMOLED 2.06
// Target: Waveshare 410x502 CO5300 / FT3168 / AXP2101 / PCF85063 / LVGL 8.4
// Single-file Arduino sketch. ExplorerOS-style staged boot and Launcher return.
// =============================================================================

#include <Arduino.h>
#include <Wire.h>
#include <FS.h>
#include <SD_MMC.h>
#include <Preferences.h>
#include <Arduino_GFX_Library.h>
#include <Arduino_DriveBus_Library.h>
#include <lvgl.h>
#include <SensorPCF85063.hpp>
#include <SensorQMI8658.hpp>
#include <ESP_I2S.h>
#define XPOWERS_CHIP_AXP2101
#include <XPowersLib.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <esp32-hal-cpu.h>
#include <time.h>

#if LVGL_VERSION_MAJOR != 8 || LVGL_VERSION_MINOR != 4
#error "VoyagerOS requires LVGL 8.4.x. Do not compile it with LVGL 9."
#endif

// =============================================================================
// 01. BUILD CONFIGURATION
// =============================================================================
#define VOYAGER_VERSION             "1.5.2"
#define VOYAGER_SAVE_VERSION        1
#define VOYAGER_POWER_SAVE          1
#define VOYAGER_AUDIO               1
#define VOYAGER_SCREEN_TIMEOUT_MS   15000UL
#define VOYAGER_WAKE_GRACE_MS       2000UL
#define VOYAGER_BOOT_HOLD_MS        1500UL
#define VOYAGER_LIGHT_SLEEP_POLL_MS 250UL
#define VOYAGER_DEFAULT_BRIGHTNESS  140
#define VOYAGER_MIN_FREE_BYTES      (8ULL * 1024ULL * 1024ULL)
#define VOYAGER_CRITICAL_FREE_BYTES (2ULL * 1024ULL * 1024ULL)

// =============================================================================
// 02. HARDWARE DEFINITIONS
// =============================================================================
#define LCD_WIDTH 410
#define LCD_HEIGHT 502
#define LCD_SDIO0 4
#define LCD_SDIO1 5
#define LCD_SDIO2 6
#define LCD_SDIO3 7
#define LCD_RESET 8
#define TP_RESET 9
#define PWR_BTN_PIN 10
#define LCD_SCLK 11
#define LCD_CS 12
#define IIC_SCL 14
#define IIC_SDA 15
#define AUDIO_DOUT 16
#define SDMMC_CMD 1
#define SDMMC_CLK 2
#define SDMMC_DATA 3
#define TP_INT 38
#define AUDIO_BCLK 41
#define AUDIO_DIN 42
#define AUDIO_MCLK 45
#define AUDIO_LRCK 40
#define AUDIO_PA 46
#define BOOT_BTN 0
#define FT3168_DEVICE_ADDRESS 0x38

static const char *PATH_ROOT       = "/VOYAGER";
static const char *PATH_JOURNAL    = "/VOYAGER/JOURNAL";
static const char *PATH_VOICE      = "/VOYAGER/VOICE";
static const char *PATH_JOURNEYS   = "/VOYAGER/JOURNEYS";
static const char *PATH_GAMES      = "/VOYAGER/GAMES";
static const char *PATH_LOCKER     = "/VOYAGER/LOCKER";
static const char *PATH_RECOVERY   = "/VOYAGER/RECOVERY";

// =============================================================================
// 03. CONSTANTS / ENUMS
// =============================================================================
enum ScreenID : uint8_t {
  SCREEN_HOME, SCREEN_JOURNAL, SCREEN_RECORD, SCREEN_TIMELINE,
  SCREEN_DAILY,
  SCREEN_JOURNEY, SCREEN_COMPANION, SCREEN_QUESTS, SCREEN_CHINESE,
  SCREEN_CHINESE_LESSON, SCREEN_CHINESE_TRACE, SCREEN_CHINESE_EXERCISE,
  SCREEN_CHINESE_DICTIONARY, SCREEN_CHINESE_PROGRESS, SCREEN_KNOWLEDGE,
  SCREEN_KNOWLEDGE_QUIZ, SCREEN_JOURNEY_HISTORY, SCREEN_BARGAIN,
  SCREEN_CURRENCY, SCREEN_PHRASES, SCREEN_LOCKER, SCREEN_PACKING,
  SCREEN_GAMES, SCREEN_RPG, SCREEN_SPACE, SCREEN_KINGDOM,
  SCREEN_CREATURES, SCREEN_HEALTH, SCREEN_SETTINGS, SCREEN_TIME,
  SCREEN_STORAGE, SCREEN_DIAGNOSTICS, SCREEN_PASS, SCREEN_TORCH, SCREEN_EMERGENCY,
  SCREEN_SAFE_MODE, SCREEN_COUNT
};

enum SleepLock : uint8_t { LOCK_NONE=0, LOCK_RECORDING=1, LOCK_TORCH=2, LOCK_GAME=4 };
enum Tone : uint8_t { TONE_1=1, TONE_2, TONE_3, TONE_4, TONE_NEUTRAL };
enum QuestKind : uint8_t { QUEST_JOURNAL, QUEST_CHINESE, QUEST_STEPS, QUEST_EXPLORE };

// =============================================================================
// 04. DATA STRUCTURES -- all types precede all globals that use them
// =============================================================================
struct SaveHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t payloadSize;
  uint32_t checksum;
};

struct JourneyState {
  char name[32];
  char country[24];
  uint32_t startEpoch;
  uint16_t plannedDays;
  bool active;
};

struct CompanionState {
  char name[16];
  uint16_t xp;
  uint8_t level;
  uint8_t mood;
  uint16_t memories;
  uint8_t energy;
  uint16_t knowledge;
  uint16_t exploration;
  uint16_t adventure;
  uint16_t strategy;
  uint8_t evolutionStage;
  uint32_t lastInteraction;
};

struct ChineseWord {
  const char *hanzi;
  const char *pinyin;
  const char *sound;
  const char *english;
  uint8_t tone;
  const char *numberedPinyin=nullptr;
  uint8_t category=0;
  uint16_t frequencyRank=0;
};

struct SrsCard {
  uint16_t wordIndex;
  uint8_t box;
  uint16_t correct;
  uint16_t wrong;
  uint32_t dueEpoch;
};

struct JournalDay {
  uint32_t date;
  uint8_t mood;
  uint16_t activityFlags;
  uint32_t steps;
  uint16_t voiceCount;
  uint16_t noteCount;
  uint16_t questsCompleted;
  uint16_t chineseReviews;
  uint16_t gameMinutes;
  bool completed;
};

struct CurrencyRate { char code[4]; float rate; uint32_t lastSetEpoch; };

struct QuestState {
  QuestKind kind;
  const char *title;
  uint16_t target;
  uint16_t progress;
  bool claimed;
};

struct GameSave {
  uint32_t coins;
  uint16_t level;
  uint16_t energy;
  uint16_t score;
};

struct TinyQuestState {uint16_t hp,maxHp,xp,gold;uint8_t level,location,weapon,armour,potions,enemyHp,bosses;uint16_t questProgress;};
struct SpaceTraderState {uint32_t credits;uint16_t fuel,cargoFood,cargoTech,cargoOre,reputation;uint8_t planet,ship,cargoCapacity,missions,piratesDefeated;uint32_t marketSeed;};
struct KingdomState {uint32_t food,wood,stone,gold,population,army;uint8_t happiness,farm,lumberyard,quarry,barracks,castle;uint32_t lastTick;};
struct CreatureState {uint32_t discovered,captured;uint16_t xp,wins,stepsBank;uint8_t activeSpecies,level,hp,maxHp,attack,defence,bait,evolution;};

struct HealthState {
  uint32_t bootCount;
  uint32_t recoveredRecordings;
  uint32_t lastError;
  uint32_t lifetimeJournalEntries;
};

struct NavState {
  ScreenID stack[12];
  uint8_t depth;
  ScreenID current;
};

// =============================================================================
// 05. STATIC CONTENT
// =============================================================================
static const ChineseWord WORDS[] = {
  {"你好","nǐ hǎo","nee how","hello",3}, {"谢谢","xiè xie","shyeh shyeh","thank you",4},
  {"请","qǐng","cheeng","please",3}, {"多少钱","duō shǎo qián","dwaw shaow chyen","how much",2},
  {"太贵了","tài guì le","tie gway luh","too expensive",4}, {"便宜一点","pián yi yì diǎn","pyen yee ee dyen","a little cheaper",2},
  {"不要","bú yào","boo yaow","do not want",2}, {"可以","kě yǐ","kuh yee","can / okay",3},
  {"水","shuǐ","shway","water",3}, {"厕所","cè suǒ","tsuh swaw","toilet",4},
  {"医院","yī yuàn","ee ywen","hospital",1}, {"帮助","bāng zhù","bahng joo","help",1},
  {"我迷路了","wǒ mí lù le","waw mee loo luh","I am lost",3}, {"在哪里","zài nǎ lǐ","dzai nah lee","where is",4},
  {"火车站","huǒ chē zhàn","hwaw chuh jan","train station",3}, {"机场","jī chǎng","jee chahng","airport",1},
  {"酒店","jiǔ diàn","jyo dyen","hotel",3}, {"菜单","cài dān","tsai dahn","menu",4},
  {"好吃","hǎo chī","how chir","delicious",3}, {"不辣","bú là","boo lah","not spicy",2},
  {"一","yī","ee","one",1}, {"二","èr","ar","two",4}, {"三","sān","sahn","three",1},
  {"四","sì","suh","four",4}, {"五","wǔ","woo","five",3}, {"六","liù","lyoh","six",4},
  {"七","qī","chee","seven",1}, {"八","bā","bah","eight",1}, {"九","jiǔ","jyoh","nine",3},
  {"十","shí","shir","ten",2}, {"今天","jīn tiān","jeen tyen","today",1},
  {"明天","míng tiān","meeng tyen","tomorrow",2}, {"朋友","péng you","pung yoh","friend",2},
  {"旅行","lǚ xíng","lyoo sheeng","travel",3}, {"安全","ān quán","ahn chwen","safe",1},
  {"现金","xiàn jīn","shyen jeen","cash",4}, {"刷卡","shuā kǎ","shwah kah","pay by card",1},
  {"左边","zuǒ biān","dzwaw byen","left",3}, {"右边","yòu biān","yoh byen","right",4},
  {"直走","zhí zǒu","jir dzoh","go straight",2}
};
static const uint16_t WORD_COUNT = sizeof(WORDS)/sizeof(WORDS[0]);

static const char *KNOWLEDGE_TITLES[] = {
  "Cambodia","China","Temple etiquette","Street food safety","Tuk-tuks",
  "Tone basics","Travel scams","Offline navigation","Jet lag","Local markets"
};
static const char *KNOWLEDGE_TEXT[] = {
  "Cambodia's capital is Phnom Penh. Khmer is the official language and the riel is used alongside US dollars.",
  "China spans one official time zone. Mandarin uses characters and four main tones plus a neutral tone.",
  "Cover shoulders and knees at active temples. Remove shoes where requested and avoid touching sacred objects.",
  "Choose busy stalls, freshly cooked hot food and sealed water. Wash hands before eating.",
  "Agree the fare and destination before leaving. Keep small notes and confirm whether the price is per person.",
  "Tone changes meaning. First is high, second rises, third dips, fourth falls sharply, and neutral is light.",
  "Pause before paying, verify tickets and change, and reject urgency. Keep passport and cards separated.",
  "Save maps and addresses before departure. Carry the destination written in the local language.",
  "Use daylight, food and movement to shift your clock. Avoid a long late-afternoon nap.",
  "Start below your true maximum, stay friendly and be willing to walk away. Never bargain over essentials after agreeing."
};
static const uint8_t KNOWLEDGE_COUNT = 10;

static const char *PHRASE_LOCAL[] = {"សួស្តី","អរគុណ","តម្លៃប៉ុន្មាន?","ថ្លៃពេក","បង្គន់នៅឯណា?","ជួយខ្ញុំផង","ខ្ញុំវង្វេងផ្លូវ"};
static const char *PHRASE_SOUND[] = {"suo-s'dei","aw-kun","tlai pon-man","tlai pek","bong-kon nov ae-na","chuoy khnyom pong","khnyom vong-veng plov"};
static const char *PHRASE_EN[] = {"Hello","Thank you","How much?","Too expensive","Where is the toilet?","Please help me","I am lost"};
static const uint8_t PHRASE_COUNT = 7;
static const char *PHRASE_ZH[] = {"你好","谢谢","多少钱？","太贵了","厕所在哪里？","请帮助我","我迷路了"};
static const char *PHRASE_ZH_SOUND[] = {"nee how","shyeh shyeh","dwaw shaow chyen","tie gway luh","tsuh swaw dzai nah lee","cheeng bahng joo waw","waw mee loo luh"};
static const char *PHRASE_JA[] = {"こんにちは","ありがとう","いくらですか？","高すぎます","トイレはどこ？","助けてください","道に迷いました"};
static const char *PHRASE_JA_SOUND[] = {"kon-nee-chee-wa","ah-ree-gah-toh","ee-koo-rah des-kah","tah-kah-soo-gee-mas","toy-reh wa doh-ko","tah-soo-keh-teh","mee-chee nee mah-yoy-mah-shta"};

// =============================================================================
// 06. GLOBAL STATE
// =============================================================================
static Arduino_DataBus *g_bus = new Arduino_ESP32QSPI(LCD_CS,LCD_SCLK,LCD_SDIO0,LCD_SDIO1,LCD_SDIO2,LCD_SDIO3);
static Arduino_GFX *g_gfx = new Arduino_CO5300(g_bus,LCD_RESET,0,LCD_WIDTH,LCD_HEIGHT,22,0,0,0);
static std::shared_ptr<Arduino_IIC_DriveBus> g_iic = std::make_shared<Arduino_HWIIC>(IIC_SDA,IIC_SCL,&Wire);
void voyager_touch_irq();
static std::unique_ptr<Arduino_IIC> g_touch(new Arduino_FT3x68(g_iic,FT3168_DEVICE_ADDRESS,DRIVEBUS_DEFAULT_VALUE,TP_INT,voyager_touch_irq));
static XPowersPMU g_pmu;
static SensorPCF85063 g_rtc;
static SensorQMI8658 g_imu;
static I2SClass g_i2s;
static Preferences g_prefs;

static lv_disp_draw_buf_t g_draw_buf;
static lv_color_t *g_lvbuf = nullptr;
static lv_disp_drv_t g_disp_drv;
static lv_indev_drv_t g_indev_drv;
static lv_indev_t *g_indev = nullptr;
static lv_obj_t *g_screen = nullptr;
static lv_obj_t *g_header_time = nullptr;
static lv_obj_t *g_status = nullptr;

static bool g_sd_ok=false, g_pmu_ok=false, g_rtc_ok=false, g_audio_ok=false, g_imu_ok=false;
static bool g_safe_mode=false;
static bool g_screen_off=false, g_touch_active=false, g_touch_block=false;
static bool g_recording=false, g_record_paused=false, g_wake_request=false, g_boot_release=false;
static bool g_returning=false;
static uint8_t g_sleep_locks=LOCK_NONE, g_brightness=VOYAGER_DEFAULT_BRIGHTNESS, g_current_brightness=0;
static uint8_t g_power_profile=0;
static uint32_t g_screen_timeout_ms=VOYAGER_SCREEN_TIMEOUT_MS;
static uint32_t g_last_touch_ms=0, g_last_wake_ms=0, g_record_started=0;
static uint32_t g_record_paused_ms=0, g_record_pause_started=0;
static uint16_t g_record_bookmarks=0;
static uint8_t g_audio_level=0, g_home_page=0;
static uint32_t g_last_save_ms=0, g_last_battery_ms=0;
static int g_battery=-1; static bool g_charging=false;
static int32_t g_touch_x=0,g_touch_y=0,g_start_x=0,g_start_y=0;
static int32_t g_trace_x=-100,g_trace_y=-100;static uint8_t g_trace_dots=0;
static uint32_t g_touch_start_ms=0;
static File g_record_file;
static char g_record_path[96]={0}, g_record_final_path[96]={0};
static NavState g_nav={{SCREEN_HOME},1,SCREEN_HOME};
static JourneyState g_journey={"My Journey","Destination",0,14,false};
static CompanionState g_companion={"Mochi",0,1,75,0,80,0,0,0,0,0,0};
static HealthState g_health={0,0,0,0};
static SrsCard g_srs[WORD_COUNT];
static GameSave g_games[4]={{100,1,100,0},{250,1,100,0},{80,1,100,0},{20,1,100,0}};
static TinyQuestState g_rpg={30,30,0,40,1,0,1,0,2,0,0,0};
static SpaceTraderState g_space={500,100,0,0,0,0,0,1,12,0,0,0x51A7E123UL};
static KingdomState g_kingdom={120,80,40,60,12,0,70,1,1,0,0,0,0};
static CreatureState g_creatures={1,1,0,0,0,0,1,20,20,5,3,3,0};
static JournalDay g_today={0,3,0,0,0,0,0,0,0,false};
static uint16_t g_packing_mask=0;
static float g_currency_rate=4100.0f;
static CurrencyRate g_rates[]={{"USD",1.0f,0},{"KHR",4100.0f,0},{"CNY",7.20f,0},{"JPY",150.0f,0},{"SGD",1.35f,0},{"THB",36.0f,0},{"AUD",1.52f,0}};
static const uint8_t RATE_COUNT=sizeof(g_rates)/sizeof(g_rates[0]);
static uint16_t g_chinese_index=0;
static int g_selected_knowledge=0;
static uint8_t g_phrase_language=0, g_quiz_answer=0, g_exercise_mode=0;
static uint32_t g_knowledge_favorites=0;
static uint8_t g_quest_claimed=0;
static uint32_t g_daily_key=0;

// =============================================================================
// 07. FORWARD DECLARATIONS
// =============================================================================
static void build_screen(ScreenID id);
static void nav_open(ScreenID id, bool push=true);
static void nav_back();
static void service_power();
static void service_audio();
static void service_header();
static void save_all();
static void load_all();
static void launcher_return_now();
static void set_screen_off(bool off);
static void show_toast(const char *msg);
static void accept_running_app();
static void launcher_cb(lv_event_t*);
static void home_cb(lv_event_t*){g_nav.depth=1;g_nav.stack[0]=SCREEN_HOME;g_nav.current=SCREEN_HOME;build_screen(SCREEN_HOME);}

// =============================================================================
// 08. UTILITY / STORAGE
// =============================================================================
static uint32_t checksum32(const uint8_t *p,size_t n){uint32_t c=2166136261UL;while(n--){c^=*p++;c*=16777619UL;}return c;}
static uint32_t now_epoch(){
  struct tm t={};
  if(getLocalTime(&t,20))return (uint32_t)mktime(&t);
  return millis()/1000;
}
static void date_key(char *out,size_t n){
  struct tm t={}; if(getLocalTime(&t,20))strftime(out,n,"%Y-%m-%d",&t); else snprintf(out,n,"DAY-%lu",millis()/86400000UL);
}
static void time_key(char *out,size_t n){
  struct tm t={}; if(getLocalTime(&t,20))strftime(out,n,"%H%M%S",&t); else snprintf(out,n,"%08lu",millis()/1000UL);
}
static bool ensure_dir(const char *p){if(!g_sd_ok)return false;if(SD_MMC.exists(p))return true;return SD_MMC.mkdir(p);}
static bool append_line(const char *path,const String &line){
  if(!g_sd_ok)return false;File f=SD_MMC.open(path,FILE_APPEND);if(!f)return false;f.println(line);f.flush();f.close();return true;
}
static bool save_blob(const char *path,uint32_t magic,const void *payload,uint16_t size){
  if(!g_sd_ok)return false;File f=SD_MMC.open(path,FILE_WRITE);if(!f)return false;SaveHeader h={magic,VOYAGER_SAVE_VERSION,size,checksum32((const uint8_t*)payload,size)};bool ok=f.write((const uint8_t*)&h,sizeof(h))==sizeof(h)&&f.write((const uint8_t*)payload,size)==size;f.flush();f.close();return ok;
}
static bool load_blob(const char *path,uint32_t magic,void *payload,uint16_t size){
  if(!g_sd_ok)return false;File f=SD_MMC.open(path,FILE_READ);if(!f)return false;SaveHeader h={};bool ok=f.read((uint8_t*)&h,sizeof(h))==sizeof(h)&&h.magic==magic&&h.version<=VOYAGER_SAVE_VERSION&&h.payloadSize==size&&f.read((uint8_t*)payload,size)==size&&h.checksum==checksum32((const uint8_t*)payload,size);f.close();return ok;
}
static void storage_init(){
  SD_MMC.setPins(SDMMC_CLK,SDMMC_CMD,SDMMC_DATA);
  for(uint8_t attempt=0;attempt<3&&!g_sd_ok;attempt++){
    g_sd_ok=SD_MMC.begin("/sdcard",true);
    if(!g_sd_ok)delay(120);
  }
  if(!g_sd_ok)return;
  ensure_dir(PATH_ROOT);ensure_dir(PATH_JOURNAL);ensure_dir(PATH_VOICE);ensure_dir(PATH_JOURNEYS);
  ensure_dir(PATH_GAMES);ensure_dir(PATH_LOCKER);ensure_dir(PATH_RECOVERY);
}
static void save_all(){
  g_prefs.begin("voyager",false);
  g_prefs.putBytes("journey",&g_journey,sizeof(g_journey));
  g_prefs.putBytes("companion",&g_companion,sizeof(g_companion));
  g_prefs.putBytes("health",&g_health,sizeof(g_health));
  g_prefs.putBytes("srs",g_srs,sizeof(g_srs));g_prefs.putBytes("games",g_games,sizeof(g_games));
  g_prefs.putBytes("rpg2",&g_rpg,sizeof(g_rpg));g_prefs.putBytes("space2",&g_space,sizeof(g_space));g_prefs.putBytes("king2",&g_kingdom,sizeof(g_kingdom));g_prefs.putBytes("creature2",&g_creatures,sizeof(g_creatures));
  g_prefs.putFloat("rate",g_currency_rate);g_prefs.putBytes("rates",g_rates,sizeof(g_rates));g_prefs.putUInt("favorites",g_knowledge_favorites);g_prefs.putUChar("quests",g_quest_claimed);g_prefs.putUInt("daykey",g_daily_key);g_prefs.putUChar("bright",g_brightness);g_prefs.putUInt("timeout",g_screen_timeout_ms);g_prefs.putUChar("profile",g_power_profile);g_prefs.putUShort("packing",g_packing_mask);g_prefs.putBytes("today",&g_today,sizeof(g_today));g_prefs.end();
  if(g_sd_ok){for(uint8_t i=0;i<4;i++){char p[48];snprintf(p,sizeof(p),"%s/GAME%u.DAT",PATH_GAMES,i);save_blob(p,0x5647414DUL+i,&g_games[i],sizeof(GameSave));}save_blob("/VOYAGER/GAMES/RPG.DAT",0x52504732UL,&g_rpg,sizeof(g_rpg));save_blob("/VOYAGER/GAMES/SPACE.DAT",0x53504332UL,&g_space,sizeof(g_space));save_blob("/VOYAGER/GAMES/KINGDOM.DAT",0x4B4E4732UL,&g_kingdom,sizeof(g_kingdom));save_blob("/VOYAGER/GAMES/CREATURE.DAT",0x43525432UL,&g_creatures,sizeof(g_creatures));}g_last_save_ms=millis();
}
static void load_all(){
  g_prefs.begin("voyager",false);
  if(g_prefs.getBytesLength("journey")==sizeof(g_journey))g_prefs.getBytes("journey",&g_journey,sizeof(g_journey));
  if(g_prefs.getBytesLength("companion")==sizeof(g_companion))g_prefs.getBytes("companion",&g_companion,sizeof(g_companion));
  if(g_prefs.getBytesLength("health")==sizeof(g_health))g_prefs.getBytes("health",&g_health,sizeof(g_health));
  if(g_prefs.getBytesLength("srs")==sizeof(g_srs))g_prefs.getBytes("srs",g_srs,sizeof(g_srs));else for(uint16_t i=0;i<WORD_COUNT;i++){g_srs[i]={i,0,0,0,0};}
  if(g_prefs.getBytesLength("games")==sizeof(g_games))g_prefs.getBytes("games",g_games,sizeof(g_games));
  if(g_prefs.getBytesLength("rpg2")==sizeof(g_rpg))g_prefs.getBytes("rpg2",&g_rpg,sizeof(g_rpg));if(g_prefs.getBytesLength("space2")==sizeof(g_space))g_prefs.getBytes("space2",&g_space,sizeof(g_space));if(g_prefs.getBytesLength("king2")==sizeof(g_kingdom))g_prefs.getBytes("king2",&g_kingdom,sizeof(g_kingdom));if(g_prefs.getBytesLength("creature2")==sizeof(g_creatures))g_prefs.getBytes("creature2",&g_creatures,sizeof(g_creatures));
  g_currency_rate=g_prefs.getFloat("rate",4100.0f);if(g_prefs.getBytesLength("rates")==sizeof(g_rates))g_prefs.getBytes("rates",g_rates,sizeof(g_rates));g_rates[1].rate=g_currency_rate;g_knowledge_favorites=g_prefs.getUInt("favorites",0);g_quest_claimed=g_prefs.getUChar("quests",0);g_daily_key=g_prefs.getUInt("daykey",0);g_brightness=g_prefs.getUChar("bright",VOYAGER_DEFAULT_BRIGHTNESS);g_screen_timeout_ms=g_prefs.getUInt("timeout",VOYAGER_SCREEN_TIMEOUT_MS);g_power_profile=g_prefs.getUChar("profile",0);g_packing_mask=g_prefs.getUShort("packing",0);if(g_prefs.getBytesLength("today")==sizeof(g_today))g_prefs.getBytes("today",&g_today,sizeof(g_today));
  uint32_t today=now_epoch()/86400UL;if(today!=g_daily_key){g_daily_key=today;g_quest_claimed=0;g_today={today,3,0,0,0,0,0,0,0,false};}
  g_health.bootCount++;g_prefs.end();if(g_sd_ok){for(uint8_t i=0;i<4;i++){char p[48];snprintf(p,sizeof(p),"%s/GAME%u.DAT",PATH_GAMES,i);GameSave tmp;if(load_blob(p,0x5647414DUL+i,&tmp,sizeof(tmp)))g_games[i]=tmp;}load_blob("/VOYAGER/GAMES/RPG.DAT",0x52504732UL,&g_rpg,sizeof(g_rpg));load_blob("/VOYAGER/GAMES/SPACE.DAT",0x53504332UL,&g_space,sizeof(g_space));load_blob("/VOYAGER/GAMES/KINGDOM.DAT",0x4B4E4732UL,&g_kingdom,sizeof(g_kingdom));load_blob("/VOYAGER/GAMES/CREATURE.DAT",0x43525432UL,&g_creatures,sizeof(g_creatures));}if(!g_kingdom.lastTick)g_kingdom.lastTick=now_epoch();uint32_t elapsed=min(21600UL,now_epoch()>g_kingdom.lastTick?now_epoch()-g_kingdom.lastTick:0UL);if(elapsed>=60){uint32_t turns=elapsed/60;g_kingdom.food+=turns*g_kingdom.farm;g_kingdom.wood+=turns*g_kingdom.lumberyard;g_kingdom.stone+=turns*g_kingdom.quarry;g_kingdom.gold+=turns*(1+g_kingdom.castle);g_kingdom.lastTick=now_epoch();}save_all();
}
static uint64_t free_sd(){return g_sd_ok?(SD_MMC.totalBytes()-SD_MMC.usedBytes()):0;}

// =============================================================================
// 09. DISPLAY / TOUCH / POWER
// =============================================================================
void voyager_touch_irq(){g_touch->IIC_Interrupt_Flag=true;}
static void flush_cb(lv_disp_drv_t *d,const lv_area_t *a,lv_color_t *px){
  g_gfx->draw16bitRGBBitmap(a->x1,a->y1,(uint16_t*)&px->full,lv_area_get_width(a),lv_area_get_height(a));lv_disp_flush_ready(d);
}
static void touch_cb(lv_indev_drv_t*,lv_indev_data_t *data){
  int32_t x=g_touch->IIC_Read_Device_Value(g_touch->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_X);
  int32_t y=g_touch->IIC_Read_Device_Value(g_touch->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_Y);
  int fingers=g_touch->IIC_Read_Device_Value(g_touch->Arduino_IIC_Touch::Value_Information::TOUCH_FINGER_NUMBER);
  bool pressed=fingers>0&&fingers<=2&&x>=0&&x<LCD_WIDTH&&y>=0&&y<LCD_HEIGHT;
  static uint8_t zero=0;
  if(pressed)zero=0;else if(g_touch_active&&++zero<3){pressed=true;x=g_touch_x;y=g_touch_y;}
  if(g_screen_off||g_touch_block){
    data->state=LV_INDEV_STATE_REL;
    if(pressed&&g_screen_off)g_wake_request=true;
    if(!pressed){g_touch_block=false;g_touch_active=false;}
    return;
  }
  if(pressed){
    data->state=LV_INDEV_STATE_PR;data->point.x=x;data->point.y=y;g_last_touch_ms=millis();
    if(g_nav.current==SCREEN_CHINESE_TRACE&&g_screen&&g_trace_dots<100&&(abs(x-g_trace_x)>5||abs(y-g_trace_y)>5)){lv_obj_t *dot=lv_obj_create(g_screen);lv_obj_set_size(dot,9,9);lv_obj_set_pos(dot,x-4,y-4);lv_obj_set_style_radius(dot,LV_RADIUS_CIRCLE,0);lv_obj_set_style_bg_color(dot,lv_color_hex(0xA78BFA),0);lv_obj_set_style_border_width(dot,0,0);lv_obj_clear_flag(dot,LV_OBJ_FLAG_SCROLLABLE);g_trace_x=x;g_trace_y=y;g_trace_dots++;}
    if(!g_touch_active){g_touch_active=true;g_start_x=x;g_start_y=y;g_touch_start_ms=millis();}
    g_touch_x=x;g_touch_y=y;
  }else{
    data->state=LV_INDEV_STATE_REL;
    if(g_touch_active){
      int dx=g_touch_x-g_start_x,dy=g_touch_y-g_start_y;uint32_t dur=millis()-g_touch_start_ms;
      g_touch_active=false;g_last_touch_ms=millis();
      if(dur<800&&abs(dx)>55&&abs(dx)>abs(dy)){if(g_nav.current==SCREEN_HOME){if(dx<0&&g_home_page<2)g_home_page++;else if(dx>0&&g_home_page>0)g_home_page--;build_screen(SCREEN_HOME);}else if(dx>0)nav_back();}
    }
  }
}
static void fade_to(uint8_t target,uint16_t ms){
  int from=g_current_brightness;int steps=10;for(int i=1;i<=steps;i++){int v=from+(target-from)*i/steps;static_cast<Arduino_CO5300*>(g_gfx)->setBrightness(v);g_current_brightness=v;lv_timer_handler();delay(ms/steps);}
}
static void set_screen_off(bool off){
  if(g_screen_off==off)return;
  g_screen_off=off;g_touch_block=true;g_wake_request=false;
  if(off)fade_to(0,180);else{lv_obj_invalidate(lv_scr_act());lv_timer_handler();fade_to(g_brightness,180);g_last_touch_ms=g_last_wake_ms=millis();}
}
static void pmu_init(){
  g_pmu_ok=g_pmu.begin(Wire,AXP2101_SLAVE_ADDRESS,IIC_SDA,IIC_SCL);
  if(!g_pmu_ok)return;g_pmu.disableTSPinMeasure();g_pmu.enableBattVoltageMeasure();
  g_pmu.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);g_pmu.clearIrqStatus();g_pmu.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ|XPOWERS_AXP2101_PKEY_LONG_IRQ);
  g_pmu.setChargingLedMode(XPOWERS_CHG_LED_OFF);
}
static void service_power(){
  uint32_t now=millis();
  if(g_wake_request)set_screen_off(false);
  static uint32_t poll=0,fire=0;if(g_pmu_ok&&now-poll>=50){poll=now;g_pmu.getIrqStatus();if(g_pmu.isPekeyShortPressIrq()&&now-fire>400){fire=now;set_screen_off(!g_screen_off);}g_pmu.clearIrqStatus();}
  if(!g_screen_off&&!g_sleep_locks&&!g_touch_active&&now-g_last_wake_ms>VOYAGER_WAKE_GRACE_MS&&now-g_last_touch_ms>=g_screen_timeout_ms)set_screen_off(true);
#if VOYAGER_POWER_SAVE
  if(g_screen_off&&!g_sleep_locks){
    esp_sleep_enable_ext1_wakeup(1ULL<<BOOT_BTN,ESP_EXT1_WAKEUP_ANY_LOW);
    esp_sleep_enable_timer_wakeup((uint64_t)VOYAGER_LIGHT_SLEEP_POLL_MS*1000ULL);esp_light_sleep_start();
    auto why=esp_sleep_get_wakeup_cause();if(why==ESP_SLEEP_WAKEUP_EXT1||digitalRead(TP_INT)==LOW)set_screen_off(false);
  }
#endif
}

// =============================================================================
// 10. RTC / AUDIO
// =============================================================================
static void rtc_init(){
  g_rtc_ok=g_rtc.begin(Wire,IIC_SDA,IIC_SCL);
  if(!g_rtc_ok)return;
  auto dt=g_rtc.getDateTime();struct tm t={};t.tm_year=dt.getYear()-1900;t.tm_mon=dt.getMonth()-1;t.tm_mday=dt.getDay();t.tm_hour=dt.getHour();t.tm_min=dt.getMinute();t.tm_sec=dt.getSecond();
  time_t e=mktime(&t);if(e>1700000000){timeval tv={e,0};settimeofday(&tv,nullptr);}
}
static bool leap_year(int y){return (y%4==0&&y%100!=0)||(y%400==0);}
static bool valid_date(int y,int mo,int d){static const uint8_t md[]={31,28,31,30,31,30,31,31,30,31,30,31};if(y<2024||y>2099||mo<1||mo>12||d<1)return false;int limit=md[mo-1]+((mo==2&&leap_year(y))?1:0);return d<=limit;}
static bool rtc_write(int y,int mo,int d,int h,int mi){
  if(!g_rtc_ok||!valid_date(y,mo,d)||h<0||h>23||mi<0||mi>59)return false;g_rtc.setDateTime(y,mo,d,h,mi,0);delay(25);auto v=g_rtc.getDateTime();
  if(v.getYear()!=y||v.getMonth()!=mo||v.getDay()!=d||v.getHour()!=h||v.getMinute()!=mi)return false;struct tm t={};t.tm_year=y-1900;t.tm_mon=mo-1;t.tm_mday=d;t.tm_hour=h;t.tm_min=mi;time_t e=mktime(&t);timeval tv={e,0};settimeofday(&tv,nullptr);return true;
}
static void audio_init(){
#if VOYAGER_AUDIO
  pinMode(AUDIO_PA,OUTPUT);digitalWrite(AUDIO_PA,HIGH);g_i2s.setPins(AUDIO_BCLK,AUDIO_LRCK,AUDIO_DOUT,AUDIO_DIN,AUDIO_MCLK);
  g_audio_ok=g_i2s.begin(I2S_MODE_STD,16000,I2S_DATA_BIT_WIDTH_16BIT,I2S_SLOT_MODE_STEREO,I2S_STD_SLOT_BOTH);
#endif
}
static void imu_init(){
  g_imu_ok=g_imu.begin(Wire,QMI8658_L_SLAVE_ADDRESS,IIC_SDA,IIC_SCL);if(!g_imu_ok)return;
  g_imu.configAccelerometer(SensorQMI8658::ACC_RANGE_2G,SensorQMI8658::ACC_ODR_62_5Hz);g_imu.enableAccelerometer();
  g_imu.configPedometer(50,200,100,200,20,10,0,4);g_imu_ok=g_imu.enablePedometer(SensorQMI8658::INTERRUPT_PIN_1);
}
static void service_imu(){
  static uint32_t last=0;if(!g_imu_ok||millis()-last<2000)return;last=millis();g_imu.update();uint32_t steps=g_imu.getPedometerCounter();if(steps>=g_today.steps)g_today.steps=steps;
}
static bool recording_start(){
  if(!g_sd_ok||!g_audio_ok||g_recording||SD_MMC.totalBytes()==0||free_sd()*100ULL/SD_MMC.totalBytes()<5ULL)return false;char d[20],t[16];date_key(d,sizeof(d));time_key(t,sizeof(t));
  char dir[72];snprintf(dir,sizeof(dir),"%s/%s",PATH_VOICE,d);ensure_dir(dir);snprintf(g_record_path,sizeof(g_record_path),"%s/%s.tmp",dir,t);snprintf(g_record_final_path,sizeof(g_record_final_path),"%s/%s.raw",dir,t);
  g_record_file=SD_MMC.open(g_record_path,FILE_WRITE);if(!g_record_file)return false;
  char marker[96];snprintf(marker,sizeof(marker),"%s/active.rec",PATH_RECOVERY);File m=SD_MMC.open(marker,FILE_WRITE);if(m){m.println(g_record_path);m.close();}
  g_recording=true;g_record_paused=false;g_record_paused_ms=0;g_record_bookmarks=0;g_record_started=millis();g_sleep_locks|=LOCK_RECORDING;return true;
}
static void recording_stop(){
  if(!g_recording)return;if(g_record_paused)g_record_paused_ms+=millis()-g_record_pause_started;g_record_file.flush();g_record_file.close();g_recording=false;g_record_paused=false;g_sleep_locks&=~LOCK_RECORDING;
  bool valid=false;if(SD_MMC.exists(g_record_path)){File check=SD_MMC.open(g_record_path,FILE_READ);valid=check&&check.size()>0;check.close();}if(valid){SD_MMC.remove(g_record_final_path);valid=SD_MMC.rename(g_record_path,g_record_final_path);}if(!valid){g_health.lastError=102;strncpy(g_record_final_path,g_record_path,sizeof(g_record_final_path));}
  SD_MMC.remove("/VOYAGER/RECOVERY/active.rec");char d[20],path[72];date_key(d,sizeof(d));snprintf(path,sizeof(path),"%s/%s.txt",PATH_JOURNAL,d);
  append_line(path,String("VOICE|")+g_record_final_path+"|"+String((millis()-g_record_started-g_record_paused_ms)/1000)+"s|BOOKMARKS="+String(g_record_bookmarks));g_today.voiceCount++;g_health.lifetimeJournalEntries++;save_all();g_last_touch_ms=millis();
}
static void service_audio(){
  if(!g_recording||g_record_paused)return;if(SD_MMC.totalBytes()&&free_sd()*100ULL/SD_MMC.totalBytes()<5ULL){g_health.lastError=103;recording_stop();return;}static uint8_t buf[2048];size_t n=g_i2s.readBytes((char*)buf,sizeof(buf));if(n){if(g_record_file.write(buf,n)!=n){g_health.lastError=101;recording_stop();}}
  if(n){int16_t *s=(int16_t*)buf;uint16_t peak=0;for(size_t i=0;i<n/2;i++){uint16_t a=abs((int)s[i]);if(a>peak)peak=a;}g_audio_level=(uint8_t)min(100,(int)(peak*100UL/32767UL));}
}
static void recover_recording(){
  if(!g_sd_ok||!SD_MMC.exists("/VOYAGER/RECOVERY/active.rec"))return;File f=SD_MMC.open("/VOYAGER/RECOVERY/active.rec",FILE_READ);String p=f.readStringUntil('\n');f.close();p.trim();
  if(p.length()&&SD_MMC.exists(p)){String recovered=p;recovered.replace(".tmp",".recovered.raw");SD_MMC.rename(p,recovered);g_health.recoveredRecordings++;}SD_MMC.remove("/VOYAGER/RECOVERY/active.rec");save_all();
}

// =============================================================================
// 11. UI HELPERS
// =============================================================================
static lv_obj_t *label(lv_obj_t *p,const char *txt,int x,int y,const lv_font_t *font=&lv_font_montserrat_16,uint32_t color=0xEAF2FF){
  lv_obj_t *o=lv_label_create(p);lv_label_set_text(o,txt);lv_obj_set_style_text_font(o,font,0);lv_obj_set_style_text_color(o,lv_color_hex(color),0);lv_obj_set_pos(o,x,y);return o;
}
static lv_obj_t *button(lv_obj_t *p,const char *txt,int x,int y,int w,int h,lv_event_cb_t cb,void *ud=nullptr,uint32_t color=0x173D52){
  lv_obj_t *b=lv_btn_create(p);lv_obj_set_pos(b,x,y);lv_obj_set_size(b,w,h);lv_obj_set_style_radius(b,16,0);lv_obj_set_style_bg_color(b,lv_color_hex(color),0);lv_obj_set_style_border_color(b,lv_color_hex(0x36D7FF),0);lv_obj_set_style_border_width(b,1,0);
  lv_obj_t *l=lv_label_create(b);lv_label_set_text(l,txt);lv_obj_set_style_text_font(l,&lv_font_montserrat_14,0);lv_obj_set_style_text_color(l,lv_color_hex(0xFFFFFF),0);lv_obj_center(l);if(cb)lv_obj_add_event_cb(b,cb,LV_EVENT_CLICKED,ud);return b;
}
static lv_obj_t *base_screen(const char *title){
  lv_obj_t *s=lv_obj_create(NULL);lv_obj_set_style_bg_color(s,lv_color_hex(0x071018),0);lv_obj_clear_flag(s,LV_OBJ_FLAG_SCROLLABLE);
  if(g_nav.depth>1)button(s,"<",10,12,48,42,[](lv_event_t*){nav_back();});label(s,title,70,18,&lv_font_montserrat_20,0xFFFFFF);
  g_header_time=label(s,"--:--",308,22,&lv_font_montserrat_14,0x67E8F9);if(g_nav.current!=SCREEN_HOME)button(s,"H",360,12,40,42,home_cb,nullptr,0x164E63);
  lv_obj_t *line=lv_obj_create(s);lv_obj_set_pos(line,14,65);lv_obj_set_size(line,382,2);lv_obj_set_style_bg_color(line,lv_color_hex(0x164E63),0);lv_obj_set_style_border_width(line,0,0);
  return s;
}
static void toast_close_cb(lv_event_t *e){lv_obj_t *b=lv_event_get_target(e);lv_obj_t *panel=lv_obj_get_parent(b);if(panel)lv_obj_del_async(panel);}
static void show_toast(const char *msg){
  if(!g_screen)return;lv_obj_t *m=lv_obj_create(g_screen);lv_obj_set_size(m,350,260);lv_obj_center(m);lv_obj_set_style_bg_color(m,lv_color_hex(0x111827),0);lv_obj_set_style_border_color(m,lv_color_hex(0x38BDF8),0);lv_obj_set_style_border_width(m,2,0);lv_obj_set_style_radius(m,18,0);lv_obj_move_foreground(m);lv_obj_t *t=label(m,"VOYAGER",12,8,&lv_font_montserrat_20,0x67E8F9);(void)t;lv_obj_t *body=label(m,msg,12,45,&lv_font_montserrat_14,0xF8FAFC);lv_obj_set_width(body,315);lv_label_set_long_mode(body,LV_LABEL_LONG_WRAP);button(m,"CLOSE",95,195,130,45,toast_close_cb,nullptr,0x334155);
}
static void nav_open(ScreenID id,bool push){
  if(push&&g_nav.depth<12)g_nav.stack[g_nav.depth++]=id;else if(!push)g_nav.stack[g_nav.depth-1]=id;g_nav.current=id;build_screen(id);
}
static void nav_back(){if(g_nav.depth<=1)return;if(g_nav.current==SCREEN_TORCH){g_sleep_locks&=~LOCK_TORCH;g_brightness=VOYAGER_DEFAULT_BRIGHTNESS;}if(g_nav.current>=SCREEN_RPG&&g_nav.current<=SCREEN_CREATURES)g_sleep_locks&=~LOCK_GAME;g_nav.depth--;g_nav.current=g_nav.stack[g_nav.depth-1];build_screen(g_nav.current);}
static void menu_cb(lv_event_t *e){nav_open((ScreenID)(intptr_t)lv_event_get_user_data(e));}
static void service_header(){
  static uint32_t last=0;if(millis()-last<1000)return;last=millis();if(g_header_time){char b[12];struct tm t={};if(getLocalTime(&t,10))strftime(b,sizeof(b),"%H:%M",&t);else strcpy(b,"--:--");lv_label_set_text(g_header_time,b);}if(g_recording&&g_status){uint32_t elapsed=millis()-g_record_started-g_record_paused_ms-(g_record_paused?millis()-g_record_pause_started:0);char d[28];snprintf(d,sizeof(d),"%02lu:%02lu  LEVEL %u%%",(unsigned long)(elapsed/60000UL),(unsigned long)(elapsed/1000UL%60UL),g_audio_level);lv_label_set_text(g_status,d);}
}

// =============================================================================
// 12. FEATURE SCREENS
// =============================================================================
static void build_home(){
  g_screen=base_screen("VOYAGER");char b[160];struct tm t={};char date[32]="OFFLINE";if(getLocalTime(&t,10))strftime(date,sizeof(date),"%a %d %b",&t);
  uint32_t day=g_journey.active?max(1UL,(now_epoch()-g_journey.startEpoch)/86400UL+1):0;
  snprintf(b,sizeof(b),"%s\n%s  %s\nDAY %lu / %u",date,g_journey.active?"TRAVELLING":"READY",g_journey.country,(unsigned long)day,g_journey.plannedDays);
  label(g_screen,b,22,84,&lv_font_montserrat_20,0xFFFFFF);
  if(g_home_page==0){button(g_screen,"Journal",18,185,116,70,menu_cb,(void*)SCREEN_JOURNAL,0x0F766E);button(g_screen,"Chinese",147,185,116,70,menu_cb,(void*)SCREEN_CHINESE,0x7C3AED);button(g_screen,"Travel",276,185,116,70,menu_cb,(void*)SCREEN_BARGAIN,0xB45309);button(g_screen,"Journey",18,270,116,70,menu_cb,(void*)SCREEN_JOURNEY);button(g_screen,"Companion",147,270,116,70,menu_cb,(void*)SCREEN_COMPANION);button(g_screen,"Games",276,270,116,70,menu_cb,(void*)SCREEN_GAMES);button(g_screen,"Knowledge",18,355,116,70,menu_cb,(void*)SCREEN_KNOWLEDGE);button(g_screen,"Health",147,355,116,70,menu_cb,(void*)SCREEN_HEALTH);button(g_screen,"Settings",276,355,116,70,menu_cb,(void*)SCREEN_SETTINGS);}
  else if(g_home_page==1){snprintf(b,sizeof(b),"TODAY\n%lu steps\n%u notes • %u voice\n%u Chinese reviews",(unsigned long)g_today.steps,g_today.noteCount,g_today.voiceCount,g_today.chineseReviews);label(g_screen,b,35,185,&lv_font_montserrat_20,0xD1FAE5);button(g_screen,"Today",35,350,160,58,menu_cb,(void*)SCREEN_DAILY,0x0F766E);button(g_screen,"Quests",215,350,160,58,menu_cb,(void*)SCREEN_QUESTS,0x6D28D9);}
  else{label(g_screen,"QUICK ACTIONS",120,165,&lv_font_montserrat_18,0xFDE68A);button(g_screen,"Record",20,205,115,55,menu_cb,(void*)SCREEN_RECORD,0x9F1239);button(g_screen,"Bargain",148,205,115,55,menu_cb,(void*)SCREEN_BARGAIN,0xB45309);button(g_screen,"Chinese",276,205,115,55,menu_cb,(void*)SCREEN_CHINESE_EXERCISE,0x7C3AED);button(g_screen,"Torch",20,275,115,55,menu_cb,(void*)SCREEN_TORCH,0xB45309);button(g_screen,"Random fact",148,275,115,55,menu_cb,(void*)SCREEN_KNOWLEDGE_QUIZ,0x155E75);button(g_screen,"Emergency",276,275,115,55,menu_cb,(void*)SCREEN_EMERGENCY,0x991B1B);button(g_screen,"BACK TO LAUNCHER",70,350,270,52,launcher_cb,nullptr,0x9F1239);}
  snprintf(b,sizeof(b),"%s   %s   %s",g_home_page==0?"●":"○",g_home_page==1?"●":"○",g_home_page==2?"●":"○");label(g_screen,b,170,438,&lv_font_montserrat_14,0x67E8F9);
  snprintf(b,sizeof(b),"BAT %s%d%%   SD %s   %s",g_charging?"+":"",g_battery,g_sd_ok?"OK":"NO",g_recording?"REC":"OFFLINE");label(g_screen,b,25,462,&lv_font_montserrat_12,0x7DD3FC);
}

static void journal_save_cb(lv_event_t *e){
  lv_obj_t *ta=(lv_obj_t*)lv_event_get_user_data(e);const char *txt=lv_textarea_get_text(ta);if(!txt||!*txt){show_toast("Write something first.");return;}
  char d[20],p[72],tm[12];date_key(d,sizeof(d));time_key(tm,sizeof(tm));snprintf(p,sizeof(p),"%s/%s.txt",PATH_JOURNAL,d);
  if(append_line(p,String("TEXT|")+tm+"|"+txt)){g_today.noteCount++;g_health.lifetimeJournalEntries++;g_companion.xp+=5;lv_textarea_set_text(ta,"");save_all();show_toast("Journal entry saved.");}else show_toast("Save failed: check SD card.");
}
static void build_journal(){
  g_screen=base_screen("Journal");lv_obj_t *ta=lv_textarea_create(g_screen);lv_obj_set_pos(ta,18,82);lv_obj_set_size(ta,374,190);lv_textarea_set_placeholder_text(ta,"What happened today?");lv_textarea_set_max_length(ta,220);
  button(g_screen,"Save entry",18,286,178,55,journal_save_cb,ta,0x0F766E);button(g_screen,"Voice",214,286,178,55,menu_cb,(void*)SCREEN_RECORD,0x9F1239);
  button(g_screen,"Today",18,355,112,55,menu_cb,(void*)SCREEN_DAILY);button(g_screen,"Timeline",148,355,112,55,menu_cb,(void*)SCREEN_TIMELINE);button(g_screen,"Prompts",278,355,112,55,[](lv_event_t*){show_toast("Best thing today?\nWhat surprised you?\nWhat made you laugh?\nWhat did you learn?\nWhat was difficult?\nWhat food did you try?\nWhere did you go?\nWho were you with?\nWhat should you remember?\nWhat would you change?\nTomorrow?");});
  label(g_screen,"Entries are grouped by date and stored on SD.",22,430,&lv_font_montserrat_12,0x94A3B8);
}
static void mood_cb(lv_event_t *e){g_today.mood=(intptr_t)lv_event_get_user_data(e);g_today.date=now_epoch()/86400UL;save_all();build_screen(SCREEN_DAILY);}
static void activity_cb(lv_event_t *e){uint8_t i=(intptr_t)lv_event_get_user_data(e);if(lv_obj_has_state(lv_event_get_target(e),LV_STATE_CHECKED))g_today.activityFlags|=(1U<<i);else g_today.activityFlags&=~(1U<<i);save_all();}
static void daily_done_cb(lv_event_t*){g_today.completed=true;g_companion.xp+=20;save_all();show_toast("Today saved. +20 XP");}
static void build_daily(){
  g_screen=base_screen("Today");label(g_screen,"HOW WAS TODAY?",95,88,&lv_font_montserrat_20,0xFFFFFF);const char *m[]={"AWFUL","LOW","OK","GOOD","GREAT"};
  for(int i=0;i<5;i++)button(g_screen,m[i],12+i*79,130,70,50,mood_cb,(void*)(intptr_t)(i+1),g_today.mood==i+1?0x047857:0x334155);
  label(g_screen,"TODAY I...",25,210,&lv_font_montserrat_18,0xFDE68A);const char *acts[]={"Explored","Travelled","Ate somewhere new","Learned","Relaxed"};for(int i=0;i<5;i++){lv_obj_t *c=lv_checkbox_create(g_screen);lv_checkbox_set_text(c,acts[i]);lv_obj_set_pos(c,28,246+i*36);if(g_today.activityFlags&(1U<<i))lv_obj_add_state(c,LV_STATE_CHECKED);lv_obj_add_event_cb(c,activity_cb,LV_EVENT_VALUE_CHANGED,(void*)(intptr_t)i);}
  button(g_screen,"SAVE DAY",110,430,190,52,daily_done_cb,nullptr,0x0F766E);
}
static void record_toggle_cb(lv_event_t*){if(g_recording){recording_stop();build_screen(SCREEN_RECORD);}else if(recording_start())build_screen(SCREEN_RECORD);else show_toast("Recorder unavailable. Check SD and audio.");}
static void record_pause_cb(lv_event_t*){if(!g_recording)return;if(g_record_paused){g_record_paused_ms+=millis()-g_record_pause_started;g_record_paused=false;}else{g_record_paused=true;g_record_pause_started=millis();}build_screen(SCREEN_RECORD);}
static void record_bookmark_cb(lv_event_t*){if(g_recording){g_record_bookmarks++;show_toast("Bookmark added.");}}
static void build_record(){
  g_screen=base_screen("Voice Journal");label(g_screen,g_recording?(g_record_paused?"PAUSED":"RECORDING"):"READY",110,105,&lv_font_montserrat_28,g_recording?0xFB7185:0x5EEAD4);
  char b[120];snprintf(b,sizeof(b),"16 kHz / 16-bit RAW\n%s",g_recording?g_record_path:(g_audio_ok?"Tap to begin":"Audio init failed"));label(g_screen,b,48,170,&lv_font_montserrat_14,0xCBD5E1);
  g_status=label(g_screen,"00:00",165,225,&lv_font_montserrat_20,0xFFFFFF);button(g_screen,g_recording?"STOP & SAVE":"START RECORDING",60,275,290,70,record_toggle_cb,nullptr,g_recording?0xBE123C:0x047857);
  if(g_recording){button(g_screen,g_record_paused?"RESUME":"PAUSE",60,355,135,52,record_pause_cb);button(g_screen,"BOOKMARK",215,355,135,52,record_bookmark_cb);}
  label(g_screen,"The screen stays awake while recording.\nA recovery marker protects interrupted files.",43,425,&lv_font_montserrat_12,0x94A3B8);
}
static void build_timeline(){
  g_screen=base_screen("Timeline");lv_obj_t *box=lv_obj_create(g_screen);lv_obj_set_pos(box,14,76);lv_obj_set_size(box,382,405);lv_obj_set_scroll_dir(box,LV_DIR_VER);
  char d[20],p[72];date_key(d,sizeof(d));snprintf(p,sizeof(p),"%s/%s.txt",PATH_JOURNAL,d);String all="TODAY\n\n";
  if(g_sd_ok){File f=SD_MMC.open(p,FILE_READ);if(f){while(f.available()&&all.length()<1800)all+=(char)f.read();f.close();}}if(all.length()<10)all+="No entries yet.";label(box,all.c_str(),8,8,&lv_font_montserrat_14,0xE2E8F0);
}

static void journey_toggle_cb(lv_event_t*){if(g_journey.active){if(g_sd_ok){char p[96],d[20];date_key(d,sizeof(d));snprintf(p,sizeof(p),"%s/%s-%s.capsule",PATH_JOURNEYS,d,g_journey.name);File f=SD_MMC.open(p,FILE_WRITE);if(f){uint32_t days=max(1UL,(now_epoch()-g_journey.startEpoch)/86400UL+1);f.printf("VOYAGER CAPSULE\n%s\n%s\nDays: %lu\nVoice: %u\nNotes: %u\nChinese reviews: %u\nQuests: %u\nCompanion XP: %u\n",g_journey.name,g_journey.country,(unsigned long)days,g_today.voiceCount,g_today.noteCount,g_today.chineseReviews,g_today.questsCompleted,g_companion.xp);f.close();}}g_journey.active=false;}else{g_journey.active=true;g_journey.startEpoch=now_epoch();g_companion.exploration+=5;}save_all();build_screen(SCREEN_JOURNEY);}
static void journey_save_cb(lv_event_t *e){lv_obj_t **a=(lv_obj_t**)lv_event_get_user_data(e);strncpy(g_journey.name,lv_textarea_get_text(a[0]),sizeof(g_journey.name)-1);g_journey.name[sizeof(g_journey.name)-1]=0;strncpy(g_journey.country,lv_textarea_get_text(a[1]),sizeof(g_journey.country)-1);g_journey.country[sizeof(g_journey.country)-1]=0;int days=atoi(lv_textarea_get_text(a[2]));g_journey.plannedDays=constrain(days,1,365);save_all();show_toast("Journey details saved.");}
static void build_journey(){
  g_screen=base_screen("Journey");char b[180];uint32_t day=g_journey.active?max(1UL,(now_epoch()-g_journey.startEpoch)/86400UL+1):0;
  snprintf(b,sizeof(b),"%s • %s\nDay %lu of %u • %s",g_journey.name,g_journey.country,(unsigned long)day,g_journey.plannedDays,g_journey.active?"ACTIVE":"READY");label(g_screen,b,25,82,&lv_font_montserrat_18,0xFFFFFF);
  static lv_obj_t *a[3];a[0]=lv_textarea_create(g_screen);lv_obj_set_pos(a[0],25,145);lv_obj_set_size(a[0],175,52);lv_textarea_set_one_line(a[0],true);lv_textarea_set_text(a[0],g_journey.name);a[1]=lv_textarea_create(g_screen);lv_obj_set_pos(a[1],215,145);lv_obj_set_size(a[1],170,52);lv_textarea_set_one_line(a[1],true);lv_textarea_set_text(a[1],g_journey.country);a[2]=lv_textarea_create(g_screen);lv_obj_set_pos(a[2],25,210);lv_obj_set_size(a[2],100,52);lv_textarea_set_one_line(a[2],true);lv_textarea_set_accepted_chars(a[2],"0123456789");char days[8];snprintf(days,sizeof(days),"%u",g_journey.plannedDays);lv_textarea_set_text(a[2],days);button(g_screen,"Save details",145,210,240,52,journey_save_cb,a);
  button(g_screen,g_journey.active?"Complete & make capsule":"Start journey",35,285,340,58,journey_toggle_cb,nullptr,g_journey.active?0x9F1239:0x047857);
  button(g_screen,"Packing",25,365,110,55,menu_cb,(void*)SCREEN_PACKING);button(g_screen,"Trip locker",150,365,110,55,menu_cb,(void*)SCREEN_LOCKER);button(g_screen,"History",275,365,110,55,menu_cb,(void*)SCREEN_JOURNEY_HISTORY);
}
static void build_journey_history(){g_screen=base_screen("Journey History");lv_obj_t *box=lv_obj_create(g_screen);lv_obj_set_pos(box,14,76);lv_obj_set_size(box,382,405);lv_obj_set_scroll_dir(box,LV_DIR_VER);String list="SAVED CAPSULES\n\n";if(g_sd_ok){File d=SD_MMC.open(PATH_JOURNEYS);if(d){File f=d.openNextFile();while(f&&list.length()<1800){if(!f.isDirectory()){list+=f.name();list+="  •  ";list+=String((unsigned long)f.size());list+=" bytes\n";}f=d.openNextFile();}d.close();}}if(list.length()<20)list+="No completed journeys yet.";lv_obj_t *l=label(box,list.c_str(),8,8,&lv_font_montserrat_14,0xE2E8F0);lv_obj_set_width(l,345);lv_label_set_long_mode(l,LV_LABEL_LONG_WRAP);}
static void build_companion(){
  g_screen=base_screen("Companion");char b[300];uint8_t level=max((int)g_companion.level,(int)(g_companion.xp/100+1));const char *form="TRAVELLER";uint16_t high=g_companion.knowledge;if(g_companion.adventure>high){high=g_companion.adventure;form="ADVENTURER";}if(g_companion.exploration>high){high=g_companion.exploration;form="EXPLORER";}if(g_companion.strategy>high)form="STRATEGIST";else if(g_companion.knowledge==high&&high>10)form="SCHOLAR";
  snprintf(b,sizeof(b),"       /\\_/\\\n      ( o.o )\n       > ^ <\n\n%s  •  Level %u\n%s FORM\nXP %u / 100\nMood %u%%  Energy %u%%\nMemories %u",g_companion.name,level,form,g_companion.xp%100,g_companion.mood,g_companion.energy,g_companion.memories);label(g_screen,b,72,85,&lv_font_montserrat_18,0xFDE68A);
  button(g_screen,"Quests",65,385,280,60,menu_cb,(void*)SCREEN_QUESTS,0x7C3AED);
}
static void quest_claim(lv_event_t *e){int i=(intptr_t)lv_event_get_user_data(e);if(g_quest_claimed&(1U<<i)){show_toast("Already claimed today.");return;}uint16_t progress[]={g_today.noteCount,g_today.chineseReviews,(uint16_t)min(65535UL,g_today.steps),(uint16_t)__builtin_popcount(g_today.activityFlags)};uint16_t target[]={1,5,1000,1};if(progress[i]<target[i]){show_toast("Quest not complete yet.");return;}g_quest_claimed|=(1U<<i);g_today.questsCompleted++;g_companion.xp+=10+i*5;g_companion.memories++;g_companion.adventure+=2;g_companion.lastInteraction=now_epoch();save_all();build_screen(SCREEN_QUESTS);}
static void build_quests(){
  g_screen=base_screen("Daily Quests");const char *q[]={"Write one journal entry","Review five Chinese words","Walk 1,000 steps","Log one activity"};uint16_t p[]={g_today.noteCount,g_today.chineseReviews,(uint16_t)min(65535UL,g_today.steps),(uint16_t)__builtin_popcount(g_today.activityFlags)};uint16_t t[]={1,5,1000,1};
  for(int i=0;i<4;i++){char b[100];snprintf(b,sizeof(b),"%s\n%u/%u  %s",q[i],p[i],t[i],g_quest_claimed&(1U<<i)?"CLAIMED":"CLAIM");button(g_screen,b,25,88+i*82,360,62,quest_claim,(void*)(intptr_t)i,p[i]>=t[i]?0x0F766E:0x4338CA);}
  char chain[90];snprintf(chain,sizeof(chain),"STORYTELLER CHAIN  %u memories\nI: 3  •  II: 10  •  III: 30",g_today.voiceCount);label(g_screen,chain,55,430,&lv_font_montserrat_12,0xFDE68A);
}

static void chinese_grade_cb(lv_event_t *e){
  int grade=(intptr_t)lv_event_get_user_data(e);SrsCard &c=g_srs[g_chinese_index];if(grade==0){c.box=0;c.wrong++;c.dueEpoch=now_epoch()+600UL;}else{c.box=min(5,(int)c.box+1);c.correct++;uint32_t delays[]={600UL,86400UL,259200UL,604800UL,1209600UL,2592000UL};c.dueEpoch=now_epoch()+delays[c.box];}g_today.chineseReviews++;g_companion.xp+=grade?3:1;g_companion.knowledge+=grade?2:1;g_companion.lastInteraction=now_epoch();save_all();g_chinese_index=(g_chinese_index+1)%WORD_COUNT;build_screen(SCREEN_CHINESE_LESSON);
}
static void build_chinese(){
  g_screen=base_screen("Chinese");uint16_t due=0;for(auto &c:g_srs)if(c.dueEpoch<=now_epoch())due++;
  char b[140];snprintf(b,sizeof(b),"%u travel words\n%u reviews due\nOffline • Hanzi • Pinyin • Sound",WORD_COUNT,due);label(g_screen,b,38,100,&lv_font_montserrat_20,0xDDD6FE);
  button(g_screen,"Start lesson",25,205,175,58,menu_cb,(void*)SCREEN_CHINESE_LESSON,0x6D28D9);button(g_screen,"Quick exercise",210,205,175,58,menu_cb,(void*)SCREEN_CHINESE_EXERCISE,0x7C3AED);
  button(g_screen,"Trace Hanzi",25,278,175,55,menu_cb,(void*)SCREEN_CHINESE_TRACE,0x4338CA);button(g_screen,"Dictionary",210,278,175,55,menu_cb,(void*)SCREEN_CHINESE_DICTIONARY,0x155E75);button(g_screen,"Progress",25,348,175,55,menu_cb,(void*)SCREEN_CHINESE_PROGRESS,0x0F766E);button(g_screen,"Phrasebook",210,348,175,55,menu_cb,(void*)SCREEN_PHRASES);
}
static void trace_next_cb(lv_event_t*){g_chinese_index=(g_chinese_index+1)%WORD_COUNT;build_screen(SCREEN_CHINESE_TRACE);}
static void build_chinese_trace(){g_screen=base_screen("Trace Hanzi");g_sleep_locks|=LOCK_GAME;g_trace_dots=0;g_trace_x=g_trace_y=-100;const ChineseWord &w=WORDS[g_chinese_index];lv_obj_t *c=label(g_screen,w.hanzi,120,105,&lv_font_montserrat_48,0x475569);lv_obj_set_style_text_opa(c,LV_OPA_60,0);label(g_screen,"Trace the reference with your finger.\nV1 provides practice without recognition.",38,220,&lv_font_montserrat_14,0xCBD5E1);button(g_screen,"CLEAR",45,350,140,60,[](lv_event_t*){build_screen(SCREEN_CHINESE_TRACE);});button(g_screen,"NEXT",225,350,140,60,trace_next_cb,nullptr,0x6D28D9);}
static void build_chinese_lesson(){
  g_screen=base_screen("Chinese Lesson");const ChineseWord &w=WORDS[g_chinese_index];char b[256];snprintf(b,sizeof(b),"%s\n\n%s\n%s\n\n%s\nTone %u • Box %u",w.hanzi,w.pinyin,w.sound,w.english,w.tone,g_srs[g_chinese_index].box);lv_obj_t *l=label(g_screen,b,0,95,&lv_font_montserrat_20,0xFFFFFF);lv_obj_set_width(l,410);lv_obj_set_style_text_align(l,LV_TEXT_ALIGN_CENTER,0);
  button(g_screen,"Again",24,365,105,58,chinese_grade_cb,(void*)0,0x9F1239);button(g_screen,"Good",152,365,105,58,chinese_grade_cb,(void*)1,0x047857);button(g_screen,"Easy",280,365,105,58,chinese_grade_cb,(void*)2,0x0369A1);
}
static void chinese_exercise_answer(lv_event_t *e){uint8_t a=(intptr_t)lv_event_get_user_data(e);SrsCard &c=g_srs[g_chinese_index];bool correct=a==g_quiz_answer;if(correct){c.box=min(5,(int)c.box+1);c.correct++;c.dueEpoch=now_epoch()+86400UL;}else{c.box=0;c.wrong++;c.dueEpoch=now_epoch()+600UL;}g_today.chineseReviews++;g_companion.knowledge+=correct?2:1;save_all();show_toast(correct?"Correct! SRS progress saved.":"Not quite. Card scheduled again.");}
static void build_chinese_exercise(){g_screen=base_screen("Chinese Exercise");g_quiz_answer=esp_random()%4;g_chinese_index=esp_random()%WORD_COUNT;g_exercise_mode=esp_random()%6;const ChineseWord &w=WORDS[g_chinese_index];const char *modes[]={"MEANING","HANZI RECOGNITION","PINYIN","ENGLISH TO CHINESE","TONE","SENTENCE ORDER"};char q[140];if(g_exercise_mode==0)snprintf(q,sizeof(q),"%s\nWhat does %s mean?",modes[g_exercise_mode],w.hanzi);else if(g_exercise_mode==1)snprintf(q,sizeof(q),"%s\nWhich means %s?",modes[g_exercise_mode],w.english);else if(g_exercise_mode==2)snprintf(q,sizeof(q),"%s\nChoose the pinyin for %s",modes[g_exercise_mode],w.hanzi);else if(g_exercise_mode==3)snprintf(q,sizeof(q),"%s\nChoose Chinese for '%s'",modes[g_exercise_mode],w.english);else if(g_exercise_mode==4)snprintf(q,sizeof(q),"%s\nWhat tone is %s?",modes[g_exercise_mode],w.pinyin);else snprintf(q,sizeof(q),"%s\nChoose a valid travel phrase",modes[g_exercise_mode]);label(g_screen,q,35,85,&lv_font_montserrat_18,0xDDD6FE);for(int i=0;i<4;i++){uint16_t wi=i==g_quiz_answer?g_chinese_index:(g_chinese_index+i+7)%WORD_COUNT;char option[90];if(g_exercise_mode==0)snprintf(option,sizeof(option),"%s",WORDS[wi].english);else if(g_exercise_mode==1||g_exercise_mode==3)snprintf(option,sizeof(option),"%s",WORDS[wi].hanzi);else if(g_exercise_mode==2)snprintf(option,sizeof(option),"%s",WORDS[wi].pinyin);else if(g_exercise_mode==4)snprintf(option,sizeof(option),"Tone %d",i+1);else snprintf(option,sizeof(option),i==g_quiz_answer?"我 → 喜欢 → 茶":"茶 → 我 → 喜欢");if(g_exercise_mode==4)g_quiz_answer=w.tone>4?0:w.tone-1;button(g_screen,option,35,185+i*65,340,50,chinese_exercise_answer,(void*)(intptr_t)i,0x4338CA);}}
static void build_chinese_dictionary(){g_screen=base_screen("Chinese Dictionary");lv_obj_t *box=lv_obj_create(g_screen);lv_obj_set_pos(box,12,76);lv_obj_set_size(box,386,410);lv_obj_set_flex_flow(box,LV_FLEX_FLOW_COLUMN);for(uint16_t i=0;i<WORD_COUNT;i++){char b[120];snprintf(b,sizeof(b),"%s  %s  —  %s",WORDS[i].hanzi,WORDS[i].pinyin,WORDS[i].english);lv_obj_t *l=label(box,b,0,0,&lv_font_montserrat_14,0xE2E8F0);lv_obj_set_width(l,345);}}
static void build_chinese_progress(){g_screen=base_screen("Chinese Progress");uint32_t correct=0,wrong=0;uint16_t learned=0,due=0;for(auto &c:g_srs){correct+=c.correct;wrong+=c.wrong;if(c.box)learned++;if(c.dueEpoch<=now_epoch())due++;}char b[260];snprintf(b,sizeof(b),"Words seen        %u / %u\nReviews due       %u\nCorrect answers   %lu\nRetry answers     %lu\nToday's reviews   %u\n\nKeep short daily sessions.\nBoxes increase review spacing.",learned,WORD_COUNT,due,(unsigned long)correct,(unsigned long)wrong,g_today.chineseReviews);label(g_screen,b,42,105,&lv_font_montserrat_18,0xD1FAE5);}

static void knowledge_pick(lv_event_t *e){g_selected_knowledge=(intptr_t)lv_event_get_user_data(e);show_toast(KNOWLEDGE_TEXT[g_selected_knowledge]);}
static void knowledge_favorite(lv_event_t*){g_knowledge_favorites^=(1UL<<g_selected_knowledge);save_all();show_toast(g_knowledge_favorites&(1UL<<g_selected_knowledge)?"Saved to favorites.":"Removed from favorites.");}
static void knowledge_search(lv_event_t *e){lv_obj_t *ta=(lv_obj_t*)lv_event_get_user_data(e);String q=lv_textarea_get_text(ta);q.toLowerCase();String hits="MATCHES\n";for(int i=0;i<KNOWLEDGE_COUNT;i++){String t=KNOWLEDGE_TITLES[i];String x=KNOWLEDGE_TEXT[i];t.toLowerCase();x.toLowerCase();if(!q.length()||t.indexOf(q)>=0||x.indexOf(q)>=0){hits+=KNOWLEDGE_TITLES[i];hits+="\n";}}show_toast(hits.c_str());}
static void build_knowledge(){
  g_screen=base_screen("Knowledge");lv_obj_t *search=lv_textarea_create(g_screen);lv_obj_set_pos(search,12,76);lv_obj_set_size(search,240,48);lv_textarea_set_one_line(search,true);lv_textarea_set_placeholder_text(search,"Search offline topics");button(g_screen,"Search",262,76,64,48,knowledge_search,search,0x155E75);button(g_screen,"Quiz",334,76,64,48,menu_cb,(void*)SCREEN_KNOWLEDGE_QUIZ,0x6D28D9);lv_obj_t *box=lv_obj_create(g_screen);lv_obj_set_pos(box,12,132);lv_obj_set_size(box,386,354);lv_obj_set_flex_flow(box,LV_FLEX_FLOW_COLUMN);lv_obj_set_style_pad_row(box,8,0);
  for(int i=0;i<KNOWLEDGE_COUNT;i++){char b[64];snprintf(b,sizeof(b),"%s%s",g_knowledge_favorites&(1UL<<i)?"★ ":"",KNOWLEDGE_TITLES[i]);lv_obj_t *o=button(box,b,0,0,350,52,knowledge_pick,(void*)(intptr_t)i);lv_obj_set_pos(o,0,0);}
}
static void knowledge_quiz_answer(lv_event_t *e){uint8_t a=(intptr_t)lv_event_get_user_data(e);show_toast(a==g_quiz_answer?"Correct!":"Try again, then open the topic to review.");if(a==g_quiz_answer){g_companion.knowledge+=2;save_all();}}
static void build_knowledge_quiz(){g_screen=base_screen("Knowledge Quiz");g_selected_knowledge=esp_random()%KNOWLEDGE_COUNT;g_quiz_answer=esp_random()%3;char q[120];snprintf(q,sizeof(q),"Which topic matches this fact?\n\n%.80s",KNOWLEDGE_TEXT[g_selected_knowledge]);label(g_screen,q,25,85,&lv_font_montserrat_16,0xE2E8F0);for(int i=0;i<3;i++){int k=i==g_quiz_answer?g_selected_knowledge:(g_selected_knowledge+i+3)%KNOWLEDGE_COUNT;button(g_screen,KNOWLEDGE_TITLES[k],35,265+i*62,340,50,knowledge_quiz_answer,(void*)(intptr_t)i,0x4338CA);}button(g_screen,"Favorite topic",110,450,190,38,knowledge_favorite,nullptr,0xB45309);}
static void bargain_calc(lv_event_t *e){
  lv_obj_t **a=(lv_obj_t**)lv_event_get_user_data(e);float ask=atof(lv_textarea_get_text(a[0]));float pct=atof(lv_textarea_get_text(a[1]));char b[160];snprintf(b,sizeof(b),"Start: %.0f\nTarget: %.0f\nWalk-away: %.0f",ask*pct/100.0f,ask*(pct+15)/100.0f,ask*(pct+25)/100.0f);lv_label_set_text(a[2],b);
}
static void build_bargain(){
  g_screen=base_screen("Bargaining");lv_obj_t *ask=lv_textarea_create(g_screen);lv_obj_set_pos(ask,25,95);lv_obj_set_size(ask,170,55);lv_textarea_set_one_line(ask,true);lv_textarea_set_accepted_chars(ask,"0123456789.");lv_textarea_set_placeholder_text(ask,"Asking price");
  lv_obj_t *pct=lv_textarea_create(g_screen);lv_obj_set_pos(pct,215,95);lv_obj_set_size(pct,170,55);lv_textarea_set_one_line(pct,true);lv_textarea_set_text(pct,"45");lv_textarea_set_accepted_chars(pct,"0123456789");
  lv_obj_t *out=label(g_screen,"Enter the asking price.",45,205,&lv_font_montserrat_20,0xFDE68A);static lv_obj_t *args[3];args[0]=ask;args[1]=pct;args[2]=out;button(g_screen,"Calculate",25,165,360,55,bargain_calc,args,0xB45309);
  button(g_screen,"Currency",25,360,170,55,menu_cb,(void*)SCREEN_CURRENCY);button(g_screen,"Phrases",215,360,170,55,menu_cb,(void*)SCREEN_PHRASES);
}
static void currency_calc(lv_event_t *e){lv_obj_t **a=(lv_obj_t**)lv_event_get_user_data(e);float amount=atof(lv_textarea_get_text(a[0]));uint8_t from=lv_dropdown_get_selected(a[2]),to=lv_dropdown_get_selected(a[3]);float result=amount/g_rates[from].rate*g_rates[to].rate;char b[100];snprintf(b,sizeof(b),"%.2f %s\n= %.2f %s",amount,g_rates[from].code,result,g_rates[to].code);lv_label_set_text(a[1],b);}
static void currency_rate_save(lv_event_t *e){lv_obj_t **a=(lv_obj_t**)lv_event_get_user_data(e);uint8_t i=lv_dropdown_get_selected(a[0]);float v=atof(lv_textarea_get_text(a[1]));if(i==0||v<=0){show_toast("Choose a non-USD currency and positive rate.");return;}g_rates[i].rate=v;g_rates[i].lastSetEpoch=now_epoch();g_currency_rate=g_rates[1].rate;save_all();show_toast("Offline rate saved. It is not live.");}
static void build_currency(){
  g_screen=base_screen("Currency");const char *opts="USD\nKHR\nCNY\nJPY\nSGD\nTHB\nAUD";label(g_screen,"OFFLINE RATES PER 1 USD",105,76,&lv_font_montserrat_14,0xFDE68A);lv_obj_t *rateCur=lv_dropdown_create(g_screen);lv_obj_set_pos(rateCur,25,105);lv_obj_set_size(rateCur,100,50);lv_dropdown_set_options(rateCur,opts);lv_dropdown_set_selected(rateCur,1);lv_obj_t *rate=lv_textarea_create(g_screen);lv_obj_set_pos(rate,135,105);lv_obj_set_size(rate,140,50);lv_textarea_set_one_line(rate,true);lv_textarea_set_accepted_chars(rate,"0123456789.");char rb[32];snprintf(rb,sizeof(rb),"%.2f",g_rates[1].rate);lv_textarea_set_text(rate,rb);static lv_obj_t *ra[2];ra[0]=rateCur;ra[1]=rate;button(g_screen,"Save",285,105,100,50,currency_rate_save,ra,0x0F766E);
  lv_obj_t *in=lv_textarea_create(g_screen);lv_obj_set_pos(in,25,185);lv_obj_set_size(in,360,55);lv_textarea_set_one_line(in,true);lv_textarea_set_accepted_chars(in,"0123456789.");lv_textarea_set_placeholder_text(in,"Amount");lv_obj_t *from=lv_dropdown_create(g_screen);lv_obj_set_pos(from,25,252);lv_obj_set_size(from,150,48);lv_dropdown_set_options(from,opts);lv_obj_t *to=lv_dropdown_create(g_screen);lv_obj_set_pos(to,235,252);lv_obj_set_size(to,150,48);lv_dropdown_set_options(to,opts);lv_dropdown_set_selected(to,1);lv_obj_t *out=label(g_screen,"",80,380,&lv_font_montserrat_20,0xFDE68A);static lv_obj_t *a[4];a[0]=in;a[1]=out;a[2]=from;a[3]=to;button(g_screen,"CONVERT",105,315,200,52,currency_calc,a,0x0369A1);label(g_screen,"All rates are editable, local, and may be outdated.",45,450,&lv_font_montserrat_12,0x94A3B8);
}
static void phrase_show(lv_event_t *e){int i=(intptr_t)lv_event_get_user_data(e);const char **local=g_phrase_language==0?PHRASE_LOCAL:g_phrase_language==1?PHRASE_ZH:PHRASE_JA;const char **sound=g_phrase_language==0?PHRASE_SOUND:g_phrase_language==1?PHRASE_ZH_SOUND:PHRASE_JA_SOUND;char b[220];snprintf(b,sizeof(b),"%s\n\n%s\n\n%s",local[i],sound[i],PHRASE_EN[i]);show_toast(b);}
static void phrase_language_cb(lv_event_t *e){g_phrase_language=lv_dropdown_get_selected(lv_event_get_target(e));build_screen(SCREEN_PHRASES);}
static void build_phrases(){g_screen=base_screen("Phrasebook");lv_obj_t *dd=lv_dropdown_create(g_screen);lv_obj_set_pos(dd,25,76);lv_obj_set_size(dd,360,48);lv_dropdown_set_options(dd,"Khmer\nMandarin\nJapanese");lv_dropdown_set_selected(dd,g_phrase_language);lv_obj_add_event_cb(dd,phrase_language_cb,LV_EVENT_VALUE_CHANGED,nullptr);const char **sound=g_phrase_language==0?PHRASE_SOUND:g_phrase_language==1?PHRASE_ZH_SOUND:PHRASE_JA_SOUND;for(int i=0;i<PHRASE_COUNT;i++){char b[96];snprintf(b,sizeof(b),"%s  •  %s",PHRASE_EN[i],sound[i]);button(g_screen,b,24,132+i*50,362,43,phrase_show,(void*)(intptr_t)i,0x155E75);}}

static void packing_cb(lv_event_t *e){uint8_t i=(intptr_t)lv_event_get_user_data(e);if(lv_obj_has_state(lv_event_get_target(e),LV_STATE_CHECKED))g_packing_mask|=(1U<<i);else g_packing_mask&=~(1U<<i);save_all();}
static void build_packing(){
  g_screen=base_screen("Packing List");const char *items[]={"Passport + copies","Cards + cash","Medicines","Chargers + adapter","Water bottle","Offline maps","Light layer"};
  for(int i=0;i<7;i++){lv_obj_t *c=lv_checkbox_create(g_screen);lv_checkbox_set_text(c,items[i]);lv_obj_set_pos(c,35,85+i*52);lv_obj_set_style_text_font(c,&lv_font_montserrat_16,0);if(g_packing_mask&(1U<<i))lv_obj_add_state(c,LV_STATE_CHECKED);lv_obj_add_event_cb(c,packing_cb,LV_EVENT_VALUE_CHANGED,(void*)(intptr_t)i);}
}
static void locker_save(lv_event_t *e){if(!g_sd_ok){show_toast("SD card unavailable.");return;}lv_obj_t *ta=(lv_obj_t*)lv_event_get_user_data(e);File f=SD_MMC.open("/VOYAGER/LOCKER/notes.txt",FILE_WRITE);if(!f){show_toast("Locker save failed.");return;}f.print(lv_textarea_get_text(ta));f.close();show_toast("Trip references saved.");}
static void build_locker(){g_screen=base_screen("Trip Locker");label(g_screen,"References only — never PINs or passwords.",28,78,&lv_font_montserrat_14,0xFDE68A);lv_obj_t *ta=lv_textarea_create(g_screen);lv_obj_set_pos(ta,20,115);lv_obj_set_size(ta,370,270);lv_textarea_set_placeholder_text(ta,"FLIGHTS\nHOTELS\nBOOKING REFERENCES\nADDRESSES\nCONTACTS\nTRANSPORT\nEMERGENCY\nOTHER");lv_textarea_set_max_length(ta,900);if(g_sd_ok){File f=SD_MMC.open("/VOYAGER/LOCKER/notes.txt",FILE_READ);if(f){String s=f.readString();f.close();lv_textarea_set_text(ta,s.c_str());}}button(g_screen,"SAVE LOCKER",75,405,260,55,locker_save,ta,0x0F766E);}

static void rpg_action(lv_event_t *e){int a=(intptr_t)lv_event_get_user_data(e);String msg;if(a==0){g_rpg.location=(g_rpg.location+1)%6;g_rpg.enemyHp=8+g_rpg.location*4+g_rpg.level*2;msg="An enemy blocks the road!";}else if(a==1&&g_rpg.enemyHp){uint8_t hit=3+g_rpg.weapon*2+esp_random()%5;g_rpg.enemyHp=hit>=g_rpg.enemyHp?0:g_rpg.enemyHp-hit;if(!g_rpg.enemyHp){uint16_t reward=8+g_rpg.location*5;g_rpg.xp+=reward;g_rpg.gold+=reward/2;g_rpg.questProgress++;if(g_rpg.location==5)g_rpg.bosses++;msg="Victory! XP and gold earned.";}else{uint8_t hurt=max(1,4+(int)g_rpg.location-(int)g_rpg.armour);g_rpg.hp=hurt>=g_rpg.hp?0:g_rpg.hp-hurt;if(!g_rpg.hp){g_rpg.hp=g_rpg.maxHp;g_rpg.gold/=2;g_rpg.location=0;g_rpg.enemyHp=0;msg="Defeated. You wake in town with half your gold.";}}}else if(a==2&&g_rpg.potions){g_rpg.potions--;g_rpg.hp=min(g_rpg.maxHp,(uint16_t)(g_rpg.hp+15));msg="Potion used.";}else if(a==3&&g_rpg.gold>=25){g_rpg.gold-=25;g_rpg.weapon=min(8,(int)g_rpg.weapon+1);msg="Weapon upgraded.";}else if(a==4&&g_rpg.gold>=20){g_rpg.gold-=20;g_rpg.potions++;msg="Potion purchased.";}else msg="You cannot do that yet.";while(g_rpg.xp>=g_rpg.level*40){g_rpg.xp-=g_rpg.level*40;g_rpg.level++;g_rpg.maxHp+=5;g_rpg.hp=g_rpg.maxHp;}g_games[0].level=g_rpg.level;g_games[0].coins=g_rpg.gold;g_games[0].score=g_rpg.questProgress;g_companion.adventure++;save_all();build_screen(SCREEN_RPG);show_toast(msg.c_str());}
static void rpg_special(lv_event_t *e){int a=(intptr_t)lv_event_get_user_data(e);String msg;if(a==0&&g_rpg.location==0&&!g_rpg.enemyHp){g_rpg.hp=g_rpg.maxHp;msg="Rested safely at the Willow Inn.";}else if(a==1&&g_rpg.gold>=30){g_rpg.gold-=30;g_rpg.armour=min(8,(int)g_rpg.armour+1);msg="Armour upgraded at the forge.";}else if(a==2&&g_rpg.questProgress>=5){g_rpg.questProgress-=5;g_rpg.gold+=60;g_rpg.xp+=30;msg="Guild contract completed: +60 gold, +30 XP.";}else msg=a==0?"Return to Willow Town to rest.":a==1?"Armour costs 30 gold.":"Defeat five enemies to complete a guild contract.";save_all();build_screen(SCREEN_RPG);show_toast(msg.c_str());}
static void build_rpg(){g_screen=base_screen("TinyQuest");g_sleep_locks|=LOCK_GAME;const char *places[]={"Willow Town","Whisper Wood","Old Mine","Frost Pass","Ruined Keep","Dragon Peak"};char b[300];snprintf(b,sizeof(b),"%s\nLV %u  HP %u/%u  XP %u/%u\nGold %u  Sword +%u  Armour +%u\nPotions %u  Contract %u/5  Bosses %u\n%s",places[g_rpg.location],g_rpg.level,g_rpg.hp,g_rpg.maxHp,g_rpg.xp,g_rpg.level*40,g_rpg.gold,g_rpg.weapon,g_rpg.armour,g_rpg.potions,g_rpg.questProgress,g_rpg.bosses,g_rpg.enemyHp?"ENEMY ENGAGED":"Choose exploration, preparation, or guild work");label(g_screen,b,22,78,&lv_font_montserrat_14,g_rpg.enemyHp?0xFCA5A5:0xD1FAE5);if(g_rpg.enemyHp){char enemy[32];snprintf(enemy,sizeof(enemy),"ATTACK • ENEMY %u HP",g_rpg.enemyHp);button(g_screen,enemy,25,245,360,52,rpg_action,(void*)1,0x9F1239);}else button(g_screen,"EXPLORE NEXT REGION",25,245,360,52,rpg_action,(void*)0,0x047857);button(g_screen,"Potion",20,310,115,45,rpg_action,(void*)2);button(g_screen,"Sword 25g",148,310,115,45,rpg_action,(void*)3,0x6D28D9);button(g_screen,"Buy potion",276,310,115,45,rpg_action,(void*)4,0xB45309);button(g_screen,"Rest",20,370,115,45,rpg_special,(void*)0,0x155E75);button(g_screen,"Armour 30g",148,370,115,45,rpg_special,(void*)1,0x4338CA);button(g_screen,"Claim guild",276,370,115,45,rpg_special,(void*)2,0x0F766E);}

static uint16_t space_price(uint8_t good){uint32_t x=g_space.marketSeed+g_space.planet*977+good*331;x^=x<<13;x^=x>>17;x^=x<<5;return (good==0?12:good==1?42:25)+(x%20);}
static void space_action(lv_event_t *e){int a=(intptr_t)lv_event_get_user_data(e);uint16_t used=g_space.cargoFood+g_space.cargoTech+g_space.cargoOre;String msg;uint8_t good=a%3;uint16_t price=space_price(good);if(a<3){if(used>=g_space.cargoCapacity||g_space.credits<price)msg="No cargo space or credits.";else{g_space.credits-=price;if(good==0)g_space.cargoFood++;else if(good==1)g_space.cargoTech++;else g_space.cargoOre++;msg="Cargo purchased.";}}else if(a<6){uint16_t *qty=good==0?&g_space.cargoFood:good==1?&g_space.cargoTech:&g_space.cargoOre;if(!*qty)msg="None in cargo.";else{(*qty)--;g_space.credits+=price;g_space.reputation++;msg="Cargo sold.";}}else if(a==6){uint16_t cost=18+g_space.planet*3;if(g_space.fuel<cost)msg="Not enough fuel.";else{g_space.fuel-=cost;g_space.planet=(g_space.planet+1)%5;g_space.marketSeed=esp_random();if(esp_random()%5==0){uint16_t loss=min((uint32_t)80,g_space.credits);g_space.credits-=loss;msg="Pirates raided the ship!";}else{g_space.missions++;g_space.reputation+=3;msg="Jump complete. Market prices changed.";}}}else if(a==7&&g_space.credits>=60){g_space.credits-=60;g_space.fuel=min(100,(int)g_space.fuel+40);msg="Fuel tanks replenished.";}else if(a==8&&g_space.credits>=250){g_space.credits-=250;g_space.ship++;g_space.cargoCapacity+=4;msg="Ship cargo bay upgraded.";}else msg="Insufficient credits.";g_games[1].coins=g_space.credits;g_games[1].score=g_space.reputation;g_companion.strategy++;save_all();build_screen(SCREEN_SPACE);show_toast(msg.c_str());}
static void space_contract(lv_event_t *e){int a=(intptr_t)lv_event_get_user_data(e);String msg;if(a==0){uint16_t cost=12+g_space.ship*3;if(g_space.fuel<cost)msg="Not enough fuel for a pirate hunt.";else{g_space.fuel-=cost;uint8_t danger=1+esp_random()%6;if(g_space.ship+esp_random()%5>=danger){g_space.piratesDefeated++;g_space.credits+=90+danger*20;g_space.reputation+=8;msg="Pirate defeated. Bounty transferred.";}else{uint32_t loss=min((uint32_t)120,g_space.credits);g_space.credits-=loss;msg="Hull damaged. Emergency repairs paid.";}}}else if(a==1){if(g_space.missions>=3){g_space.missions-=3;g_space.credits+=180;g_space.reputation+=12;msg="Three delivery stamps exchanged for a contract bonus.";}else msg="Complete three safe jumps for a delivery bonus.";}else{uint16_t total=g_space.cargoFood+g_space.cargoTech+g_space.cargoOre;if(total<3)msg="Carry at least three cargo units.";else{uint32_t value=g_space.cargoFood*space_price(0)+g_space.cargoTech*space_price(1)+g_space.cargoOre*space_price(2);g_space.credits+=value;g_space.reputation+=total;g_space.cargoFood=g_space.cargoTech=g_space.cargoOre=0;msg="Entire cargo manifest sold at current prices.";}}save_all();build_screen(SCREEN_SPACE);show_toast(msg.c_str());}
static void build_space(){g_screen=base_screen("Space Trader");g_sleep_locks|=LOCK_GAME;const char *p[]={"Terra","Cinder","Pelagos","Nimbus","Frontier"};char b[320];snprintf(b,sizeof(b),"ORBIT %s • Ship Mk %u • Rep %u\nCredits %lu  Fuel %u/100  Cargo %u/%u\nFood %u @%u  Tech %u @%u  Ore %u @%u\nDelivery stamps %u  Pirate wins %u",p[g_space.planet],g_space.ship,g_space.reputation,(unsigned long)g_space.credits,g_space.fuel,g_space.cargoFood+g_space.cargoTech+g_space.cargoOre,g_space.cargoCapacity,g_space.cargoFood,space_price(0),g_space.cargoTech,space_price(1),g_space.cargoOre,space_price(2),g_space.missions,g_space.piratesDefeated);label(g_screen,b,20,78,&lv_font_montserrat_14,0xBAE6FD);const char *buy[]={"BUY FOOD","BUY TECH","BUY ORE"};const char *sell[]={"SELL FOOD","SELL TECH","SELL ORE"};for(int i=0;i<3;i++){button(g_screen,buy[i],20+i*130,205,115,42,space_action,(void*)(intptr_t)i,0x155E75);button(g_screen,sell[i],20+i*130,255,115,42,space_action,(void*)(intptr_t)(i+3),0x0F766E);}button(g_screen,"JUMP",20,310,115,44,space_action,(void*)6,0x6D28D9);button(g_screen,"REFUEL",148,310,115,44,space_action,(void*)7,0xB45309);button(g_screen,"SHIP +",276,310,115,44,space_action,(void*)8,0x4338CA);button(g_screen,"PIRATE HUNT",20,365,115,44,space_contract,(void*)0,0x9F1239);button(g_screen,"CONTRACT",148,365,115,44,space_contract,(void*)1,0x7C3AED);button(g_screen,"SELL ALL",276,365,115,44,space_contract,(void*)2,0x047857);}

static void kingdom_action(lv_event_t *e){int a=(intptr_t)lv_event_get_user_data(e);String msg;if(a==0){g_kingdom.food+=15;g_kingdom.wood+=8;g_kingdom.stone+=4;msg="Workers gathered supplies.";}else if(a==1&&g_kingdom.wood>=40&&g_kingdom.stone>=20){g_kingdom.wood-=40;g_kingdom.stone-=20;g_kingdom.farm++;msg="Farm upgraded: more offline food.";}else if(a==2&&g_kingdom.wood>=35&&g_kingdom.gold>=20){g_kingdom.wood-=35;g_kingdom.gold-=20;g_kingdom.barracks++;g_kingdom.army+=5;msg="Barracks trained five guards.";}else if(a==3&&g_kingdom.food>=30){g_kingdom.food-=30;g_kingdom.happiness=min(100,(int)g_kingdom.happiness+8);g_kingdom.population+=2;msg="Festival raised happiness and population.";}else if(a==4&&g_kingdom.stone>=80&&g_kingdom.gold>=100){g_kingdom.stone-=80;g_kingdom.gold-=100;g_kingdom.castle++;msg="Castle expanded. Gold production increased.";}else msg="Not enough resources.";g_kingdom.lastTick=now_epoch();g_games[2].coins=g_kingdom.gold;g_games[2].score=g_kingdom.population;g_companion.strategy++;save_all();build_screen(SCREEN_KINGDOM);show_toast(msg.c_str());}
static void kingdom_develop(lv_event_t *e){int a=(intptr_t)lv_event_get_user_data(e);String msg;if(a==0&&g_kingdom.wood>=35&&g_kingdom.gold>=15){g_kingdom.wood-=35;g_kingdom.gold-=15;g_kingdom.lumberyard++;msg="Lumberyard upgraded: more offline wood.";}else if(a==1&&g_kingdom.wood>=25&&g_kingdom.gold>=25){g_kingdom.wood-=25;g_kingdom.gold-=25;g_kingdom.quarry++;msg="Quarry upgraded: more offline stone.";}else if(a==2){uint32_t tax=g_kingdom.population*(2+g_kingdom.castle);g_kingdom.gold+=tax;g_kingdom.happiness=g_kingdom.happiness>5?g_kingdom.happiness-5:0;msg="Taxes collected. Happiness fell by 5.";}else if(a==3&&g_kingdom.food>=20&&g_kingdom.gold>=10){g_kingdom.food-=20;g_kingdom.gold-=10;g_kingdom.army+=3+g_kingdom.barracks;msg="New guards joined the army.";}else msg="Not enough resources for this development.";g_kingdom.lastTick=now_epoch();save_all();build_screen(SCREEN_KINGDOM);show_toast(msg.c_str());}
static void build_kingdom(){g_screen=base_screen("Pocket Kingdom");g_sleep_locks|=LOCK_GAME;char b[340];snprintf(b,sizeof(b),"REALM TIER %u • Pop %lu • Happy %u%% • Army %lu\nFood %lu  Wood %lu  Stone %lu  Gold %lu\nProduction/min: +%u food +%u wood +%u stone +%u gold\nFarm %u  Lumber %u  Quarry %u  Barracks %u  Castle %u",g_kingdom.castle+1,(unsigned long)g_kingdom.population,g_kingdom.happiness,(unsigned long)g_kingdom.army,(unsigned long)g_kingdom.food,(unsigned long)g_kingdom.wood,(unsigned long)g_kingdom.stone,(unsigned long)g_kingdom.gold,g_kingdom.farm,g_kingdom.lumberyard,g_kingdom.quarry,1+g_kingdom.castle,g_kingdom.farm,g_kingdom.lumberyard,g_kingdom.quarry,g_kingdom.barracks,g_kingdom.castle);label(g_screen,b,18,78,&lv_font_montserrat_14,0xFDE68A);button(g_screen,"Gather",20,225,115,42,kingdom_action,(void*)0,0x0F766E);button(g_screen,"Farm",148,225,115,42,kingdom_action,(void*)1,0x047857);button(g_screen,"Festival",276,225,115,42,kingdom_action,(void*)3,0xB45309);button(g_screen,"Lumber",20,278,115,42,kingdom_develop,(void*)0,0x155E75);button(g_screen,"Quarry",148,278,115,42,kingdom_develop,(void*)1,0x334155);button(g_screen,"Recruit",276,278,115,42,kingdom_develop,(void*)3,0x4338CA);button(g_screen,"Barracks",20,335,115,42,kingdom_action,(void*)2,0x4338CA);button(g_screen,"Collect tax",148,335,115,42,kingdom_develop,(void*)2,0x9F1239);button(g_screen,"Castle",276,335,115,42,kingdom_action,(void*)4,0x6D28D9);label(g_screen,"Offline gains cap at six hours; clock rollback gives no reward.",34,410,&lv_font_montserrat_12,0x94A3B8);}

static void creature_action(lv_event_t *e){int a=(intptr_t)lv_event_get_user_data(e);String msg;if(a==0){uint32_t delta=g_today.steps>g_creatures.stepsBank?g_today.steps-g_creatures.stepsBank:0;if(delta<100&&!g_creatures.bait){msg="Walk 100 more steps or earn bait in quests.";}else{if(delta>=100)g_creatures.stepsBank+=100;else g_creatures.bait--;uint8_t species=esp_random()%16;g_creatures.discovered|=(1UL<<species);if(esp_random()%100<55+g_creatures.level*2){g_creatures.captured|=(1UL<<species);g_creatures.activeSpecies=species;msg="Creature captured and made active!";}else msg="Discovered, but it escaped.";}}else if(a==1){uint8_t foe=2+esp_random()%12;uint8_t hit=max(1,(int)g_creatures.attack-(int)(foe/3));uint8_t hurt=max(1,(int)foe-(int)g_creatures.defence);if(hit*3>=foe*2){g_creatures.wins++;g_creatures.xp+=12+foe;g_creatures.hp=g_creatures.hp>hurt?g_creatures.hp-hurt:1;msg="Battle won!";}else{g_creatures.hp=g_creatures.hp>hurt*2?g_creatures.hp-hurt*2:1;msg="The wild creature was stronger.";}}else if(a==2){g_creatures.hp=g_creatures.maxHp;msg="Active creature rested.";}else if(a==3&&g_creatures.level>=5&&g_creatures.evolution<3){g_creatures.evolution++;g_creatures.attack+=3;g_creatures.defence+=2;msg="Evolution complete!";}else msg="Reach level 5 or next evolution threshold.";while(g_creatures.xp>=g_creatures.level*30){g_creatures.xp-=g_creatures.level*30;g_creatures.level++;g_creatures.maxHp+=4;g_creatures.hp=g_creatures.maxHp;g_creatures.attack++;if(g_creatures.level%5==0)g_creatures.bait++;}g_games[3].level=g_creatures.level;g_games[3].score=g_creatures.wins;g_companion.exploration++;save_all();build_screen(SCREEN_CREATURES);show_toast(msg.c_str());}
static void creature_manage(lv_event_t *e){int a=(intptr_t)lv_event_get_user_data(e);String msg;if(a==0){uint8_t start=g_creatures.activeSpecies;do{g_creatures.activeSpecies=(g_creatures.activeSpecies+1)%16;}while(!(g_creatures.captured&(1UL<<g_creatures.activeSpecies))&&g_creatures.activeSpecies!=start);msg=g_creatures.activeSpecies==start?"Capture another species to switch.":"Active creature changed.";}else if(a==1&&g_creatures.bait){g_creatures.bait--;g_creatures.xp+=20;g_creatures.attack++;msg="Training consumed one bait: +20 XP, +1 attack.";}else if(a==2&&g_creatures.wins>=5){g_creatures.wins-=5;g_creatures.bait+=2;g_creatures.defence++;msg="Arena reward: two bait and +1 defence.";}else msg=a==1?"Training requires bait.":"Win five battles to claim an arena reward.";save_all();build_screen(SCREEN_CREATURES);show_toast(msg.c_str());}
static void build_creatures(){g_screen=base_screen("Creature Collector");g_sleep_locks|=LOCK_GAME;static const char *species[]={"Mossling","Emberfox","Tidefin","Cloudlet","Pebbleon","Voltkit","Moonmoth","Thorncub","Glider","Frostbit","Sunscale","Echoowl","Duneling","Bloomtail","Starhorn","Voidpup"};uint32_t available=g_today.steps>g_creatures.stepsBank?g_today.steps-g_creatures.stepsBank:0;char b[340];snprintf(b,sizeof(b),"ACTIVE %s • Evo %u • LV %u\nHP %u/%u  XP %u/%u  ATK %u  DEF %u\nWins %u  Bait %u  Walk credit %lu/100\nCollection: %u discovered • %u captured",species[g_creatures.activeSpecies%16],g_creatures.evolution,g_creatures.level,g_creatures.hp,g_creatures.maxHp,g_creatures.xp,g_creatures.level*30,g_creatures.attack,g_creatures.defence,g_creatures.wins,g_creatures.bait,(unsigned long)min(available,100UL),__builtin_popcount(g_creatures.discovered),__builtin_popcount(g_creatures.captured));label(g_screen,b,20,78,&lv_font_montserrat_14,0xDDD6FE);button(g_screen,"SEARCH",20,225,115,45,creature_action,(void*)0,0x0F766E);button(g_screen,"BATTLE",148,225,115,45,creature_action,(void*)1,0x9F1239);button(g_screen,"REST",276,225,115,45,creature_action,(void*)2,0x155E75);button(g_screen,"NEXT",20,285,115,45,creature_manage,(void*)0,0x4338CA);button(g_screen,"TRAIN",148,285,115,45,creature_manage,(void*)1,0xB45309);button(g_screen,"EVOLVE",276,285,115,45,creature_action,(void*)3,0x7C3AED);button(g_screen,"CLAIM ARENA REWARD",55,350,300,48,creature_manage,(void*)2,0x6D28D9);label(g_screen,"Search uses 100 new steps or one bait.",70,420,&lv_font_montserrat_12,0x94A3B8);}

static void kingdom_refresh(){uint32_t now=now_epoch(),elapsed=min(21600UL,now>g_kingdom.lastTick?now-g_kingdom.lastTick:0UL);if(elapsed>=60){uint32_t turns=elapsed/60;g_kingdom.food+=turns*g_kingdom.farm;g_kingdom.wood+=turns*g_kingdom.lumberyard;g_kingdom.stone+=turns*g_kingdom.quarry;g_kingdom.gold+=turns*(1+g_kingdom.castle);g_kingdom.lastTick=now;save_all();}}
static void build_game(int i,const char*,const char*){if(i==0)build_rpg();else if(i==1)build_space();else if(i==2){kingdom_refresh();build_kingdom();}else build_creatures();}
static void game_open(lv_event_t *e){nav_open((ScreenID)(SCREEN_RPG+(intptr_t)lv_event_get_user_data(e)));}
static void build_games(){g_sleep_locks&=~LOCK_GAME;g_screen=base_screen("Games");char a[80],b[80],c[80],d[80];snprintf(a,sizeof(a),"TinyQuest • LV %u • %u contracts",g_rpg.level,g_rpg.questProgress);snprintf(b,sizeof(b),"Space Trader • %lu credits • Rep %u",(unsigned long)g_space.credits,g_space.reputation);snprintf(c,sizeof(c),"Pocket Kingdom • Pop %lu • Tier %u",(unsigned long)g_kingdom.population,g_kingdom.castle+1);snprintf(d,sizeof(d),"Creatures • %u/16 • LV %u",__builtin_popcount(g_creatures.captured),g_creatures.level);const char *n[]={a,b,c,d};for(int i=0;i<4;i++)button(g_screen,n[i],30,82+i*75,350,56,game_open,(void*)(intptr_t)i,0x4338CA);button(g_screen,"Pass the Watch",30,390,350,56,menu_cb,(void*)SCREEN_PASS,0x155E75);}

static void build_health(){
  g_screen=base_screen("Watch Health");char b[500];uint64_t total=g_sd_ok?SD_MMC.totalBytes():0,free=free_sd();
  snprintf(b,sizeof(b),"Battery       %d%% %s\nSD card       %s\nSD free       %.1f MB / %.1f MB\nHeap free     %lu KB\nPSRAM free    %lu KB\nBoot count    %lu\nRecovered RAW %lu\nJournal items %lu\nAudio         %s\nRTC           %s\nLast error    %lu",g_battery,g_charging?"CHARGING":"",g_sd_ok?"OK":"MISSING",free/1048576.0,total/1048576.0,(unsigned long)(ESP.getFreeHeap()/1024),(unsigned long)(ESP.getFreePsram()/1024),(unsigned long)g_health.bootCount,(unsigned long)g_health.recoveredRecordings,(unsigned long)g_health.lifetimeJournalEntries,g_audio_ok?"READY":"OFFLINE",g_rtc_ok?"READY":"OFFLINE",(unsigned long)g_health.lastError);label(g_screen,b,32,82,&lv_font_montserrat_16,0xD1FAE5);button(g_screen,"Diagnostics",25,420,170,50,menu_cb,(void*)SCREEN_DIAGNOSTICS);button(g_screen,"Storage",215,420,170,50,menu_cb,(void*)SCREEN_STORAGE);
}
static uint64_t dir_bytes(const char *path){if(!g_sd_ok)return 0;File d=SD_MMC.open(path);if(!d)return 0;uint64_t total=0;File f=d.openNextFile();while(f){if(f.isDirectory())total+=dir_bytes(f.path());else total+=f.size();f=f.openNextFile();}d.close();return total;}
static void build_storage(){g_screen=base_screen("Storage");uint64_t total=g_sd_ok?SD_MMC.totalBytes():0,free=free_sd(),j=dir_bytes(PATH_JOURNAL)+dir_bytes(PATH_VOICE),ga=dir_bytes(PATH_GAMES),lo=dir_bytes(PATH_LOCKER);float pct=total?free*100.0f/total:0;char b[360];snprintf(b,sizeof(b),"Journal + voice  %.2f MB\nGames + saves    %.2f MB\nTrip locker      %.2f MB\n\nFREE             %.2f MB (%.1f%%)\n\n%s",j/1048576.0,ga/1048576.0,lo/1048576.0,free/1048576.0,pct,pct<5?"CRITICAL: VOICE RECORDING DISABLED":pct<10?"SERIOUS STORAGE WARNING":pct<20?"STORAGE WARNING":"Storage level healthy");label(g_screen,b,32,100,&lv_font_montserrat_18,pct<10?0xFDA4AF:0xBAE6FD);}
static void diagnostic_test(lv_event_t *e){int i=(intptr_t)lv_event_get_user_data(e);char b[180];if(i==0)snprintf(b,sizeof(b),"STORAGE: %s\nFree: %.1f MB",g_sd_ok?"PASS":"FAIL",free_sd()/1048576.0);else if(i==1)snprintf(b,sizeof(b),"SENSORS\nPMU %s\nRTC %s\nIMU %s",g_pmu_ok?"PASS":"FAIL",g_rtc_ok?"PASS":"FAIL",g_imu_ok?"PASS":"FAIL");else snprintf(b,sizeof(b),"AUDIO %s\nTouch input is active if you opened this test.\nDisplay rendering: PASS",g_audio_ok?"PASS":"FAIL");show_toast(b);}
static void build_diagnostics(){g_screen=base_screen("Diagnostics");char b[420];snprintf(b,sizeof(b),"Firmware %s • LVGL %d.%d.%d\nCPU %lu MHz • Heap %lu KB • PSRAM %lu KB\nSD %s • PMU %s • RTC %s • IMU %s • I2S %s",VOYAGER_VERSION,LVGL_VERSION_MAJOR,LVGL_VERSION_MINOR,LVGL_VERSION_PATCH,(unsigned long)getCpuFrequencyMhz(),(unsigned long)(ESP.getFreeHeap()/1024),(unsigned long)(ESP.getFreePsram()/1024),g_sd_ok?"OK":"FAIL",g_pmu_ok?"OK":"FAIL",g_rtc_ok?"OK":"FAIL",g_imu_ok?"OK":"FAIL",g_audio_ok?"OK":"FAIL");lv_obj_t *l=label(g_screen,b,25,85,&lv_font_montserrat_14,0xBAE6FD);lv_obj_set_width(l,360);button(g_screen,"Test storage",35,245,340,52,diagnostic_test,(void*)0);button(g_screen,"Test sensors",35,315,340,52,diagnostic_test,(void*)1);button(g_screen,"Test display / touch / audio",35,385,340,52,diagnostic_test,(void*)2,0x0F766E);}

static void pass_action(lv_event_t *e){int i=(intptr_t)lv_event_get_user_data(e);char b[180];switch(i){case 0:snprintf(b,sizeof(b),"DICE: %u",(unsigned)(esp_random()%6+1));break;case 1:snprintf(b,sizeof(b),"COIN: %s",esp_random()&1?"HEADS":"TAILS");break;case 2:snprintf(b,sizeof(b),"NUMBER: %u",(unsigned)(esp_random()%100+1));break;case 3:{const char *c[]={"Act like a tour guide","Mime an animal","Describe a place without naming it","Name five foods","Tell a two-line story"};snprintf(b,sizeof(b),"CHALLENGE\n%s",c[esp_random()%5]);break;}default:{const char *q[]={"Would you rather explore space or the ocean?","Who am I? Choose a famous person.","Name three things beginning with S.","What fact surprised you today?"};snprintf(b,sizeof(b),"%s",q[esp_random()%4]);break;}}show_toast(b);}
static void build_pass(){g_screen=base_screen("Pass the Watch");const char *n[]={"Roll dice","Flip coin","Random 1–100","Quick challenge","Trivia / categories"};for(int i=0;i<5;i++)button(g_screen,n[i],35,90+i*70,340,55,pass_action,(void*)(intptr_t)i,i<3?0x155E75:0x6D28D9);}

static void time_save_cb(lv_event_t *e){lv_obj_t **a=(lv_obj_t**)lv_event_get_user_data(e);int y=atoi(lv_textarea_get_text(a[0])),mo=atoi(lv_textarea_get_text(a[1])),d=atoi(lv_textarea_get_text(a[2])),h=atoi(lv_textarea_get_text(a[3])),mi=atoi(lv_textarea_get_text(a[4]));if(!valid_date(y,mo,d)||h<0||h>23||mi<0||mi>59){show_toast("Invalid date or time.");return;}if(rtc_write(y,mo,d,h,mi))show_toast("RTC updated and verified.");else{g_health.lastError=202;show_toast("RTC write verification failed.");}}
static void build_time(){
  g_screen=base_screen("Date & Time");struct tm t={};getLocalTime(&t,10);const char *ph[]={"YYYY","MM","DD","HH","MIN"};static lv_obj_t *a[5];int vals[]={t.tm_year+1900,t.tm_mon+1,t.tm_mday,t.tm_hour,t.tm_min};
  for(int i=0;i<5;i++){a[i]=lv_textarea_create(g_screen);lv_obj_set_pos(a[i],20+i*76,125);lv_obj_set_size(a[i],66,58);lv_textarea_set_one_line(a[i],true);lv_textarea_set_accepted_chars(a[i],"0123456789");char b[8];snprintf(b,sizeof(b),i?"%02d":"%04d",vals[i]);lv_textarea_set_text(a[i],b);label(g_screen,ph[i],22+i*76,100,&lv_font_montserrat_12,0x94A3B8);}
  button(g_screen,"Save to RTC",45,225,320,65,time_save_cb,a,0x047857);label(g_screen,"Changing time may make SRS reviews immediately due.\nGame saves use elapsed progress, not wall-clock rewards.",30,330,&lv_font_montserrat_14,0xFDE68A);
}
static void torch_exit(lv_event_t*){g_sleep_locks&=~LOCK_TORCH;g_brightness=VOYAGER_DEFAULT_BRIGHTNESS;nav_back();}
static void build_torch(){g_sleep_locks|=LOCK_TORCH;g_screen=lv_obj_create(NULL);lv_obj_set_style_bg_color(g_screen,lv_color_hex(0xFFFFFF),0);button(g_screen,"EXIT TORCH",95,420,220,55,torch_exit,nullptr,0x111827);static_cast<Arduino_CO5300*>(g_gfx)->setBrightness(255);g_current_brightness=255;}
static void emergency_save(lv_event_t *e){if(!g_sd_ok){show_toast("SD card unavailable.");return;}File f=SD_MMC.open("/VOYAGER/LOCKER/emergency.txt",FILE_WRITE);if(!f){show_toast("Emergency info save failed.");return;}f.print(lv_textarea_get_text((lv_obj_t*)lv_event_get_user_data(e)));f.close();show_toast("Emergency information saved offline.");}
static void build_emergency(){g_screen=base_screen("EMERGENCY");label(g_screen,"112 / LOCAL EMERGENCY SERVICES",45,78,&lv_font_montserrat_16,0xFCA5A5);lv_obj_t *ta=lv_textarea_create(g_screen);lv_obj_set_pos(ta,20,112);lv_obj_set_size(ta,370,220);lv_textarea_set_placeholder_text(ta,"Name, medical needs, emergency contact, hotel/address, insurance reference");lv_textarea_set_max_length(ta,700);if(g_sd_ok){File f=SD_MMC.open("/VOYAGER/LOCKER/emergency.txt",FILE_READ);if(f){String s=f.readString();f.close();lv_textarea_set_text(ta,s.c_str());}}button(g_screen,"SAVE INFO",20,350,175,55,emergency_save,ta,0x0F766E);button(g_screen,"TORCH",215,350,175,55,menu_cb,(void*)SCREEN_TORCH,0xB91C1C);label(g_screen,"Keep critical details brief and readable.",55,430,&lv_font_montserrat_12,0xFDE68A);}

static void safe_continue(lv_event_t*){g_safe_mode=false;accept_running_app();build_screen(SCREEN_HOME);}
static void build_safe_mode(){g_screen=base_screen("SAFE MODE");label(g_screen,"Voyager detected repeated incomplete boots.\n\nStorage and saved data were not erased.\nUse Continue to try the app with diagnostics,\nor return to LauncherOS.",30,100,&lv_font_montserrat_16,0xFDE68A);button(g_screen,"CONTINUE",35,300,340,58,safe_continue,nullptr,0x0F766E);button(g_screen,"DIAGNOSTICS",35,370,160,55,menu_cb,(void*)SCREEN_DIAGNOSTICS);button(g_screen,"LAUNCHEROS",215,370,160,55,launcher_cb,nullptr,0x9F1239);}

static void launcher_cb(lv_event_t*){save_all();launcher_return_now();}
static void deep_sleep_cb(lv_event_t*){save_all();if(g_recording)recording_stop();fade_to(0,220);esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH,ESP_PD_OPTION_ON);esp_sleep_enable_ext1_wakeup(1ULL<<BOOT_BTN,ESP_EXT1_WAKEUP_ANY_LOW);esp_deep_sleep_start();}
static void brightness_cb(lv_event_t *e){g_brightness=lv_slider_get_value(lv_event_get_target(e));g_current_brightness=g_brightness;static_cast<Arduino_CO5300*>(g_gfx)->setBrightness(g_brightness);save_all();}
static void timeout_cb(lv_event_t *e){static const uint32_t opts[]={10000,15000,30000,60000};g_screen_timeout_ms=opts[lv_dropdown_get_selected(lv_event_get_target(e))];save_all();}
static void profile_cb(lv_event_t *e){g_power_profile=lv_dropdown_get_selected(lv_event_get_target(e));if(g_power_profile==0){g_brightness=140;g_screen_timeout_ms=15000;}else if(g_power_profile==1){g_brightness=100;g_screen_timeout_ms=15000;}else{g_brightness=70;g_screen_timeout_ms=10000;}g_current_brightness=g_brightness;static_cast<Arduino_CO5300*>(g_gfx)->setBrightness(g_brightness);save_all();build_screen(SCREEN_SETTINGS);}
static void build_settings(){
  g_screen=base_screen("Settings");label(g_screen,"Brightness",26,78,&lv_font_montserrat_14,0xFFFFFF);lv_obj_t *sl=lv_slider_create(g_screen);lv_obj_set_pos(sl,25,105);lv_obj_set_size(sl,360,18);lv_slider_set_range(sl,20,255);lv_slider_set_value(sl,g_brightness,LV_ANIM_OFF);lv_obj_add_event_cb(sl,brightness_cb,LV_EVENT_VALUE_CHANGED,nullptr);
  lv_obj_t *to=lv_dropdown_create(g_screen);lv_obj_set_pos(to,25,140);lv_obj_set_size(to,170,48);lv_dropdown_set_options(to,"10 seconds\n15 seconds\n30 seconds\n60 seconds");uint8_t ti=g_screen_timeout_ms==10000?0:g_screen_timeout_ms==30000?2:g_screen_timeout_ms==60000?3:1;lv_dropdown_set_selected(to,ti);lv_obj_add_event_cb(to,timeout_cb,LV_EVENT_VALUE_CHANGED,nullptr);
  lv_obj_t *pr=lv_dropdown_create(g_screen);lv_obj_set_pos(pr,215,140);lv_obj_set_size(pr,170,48);lv_dropdown_set_options(pr,"Normal\nTravel\nMaximum");lv_dropdown_set_selected(pr,g_power_profile);lv_obj_add_event_cb(pr,profile_cb,LV_EVENT_VALUE_CHANGED,nullptr);
  button(g_screen,"Date & time",25,205,170,50,menu_cb,(void*)SCREEN_TIME);button(g_screen,"Storage",215,205,170,50,menu_cb,(void*)SCREEN_STORAGE);
  button(g_screen,"Emergency",25,270,170,50,menu_cb,(void*)SCREEN_EMERGENCY,0x991B1B);button(g_screen,"Deep sleep",215,270,170,50,deep_sleep_cb);
  button(g_screen,"BACK TO LAUNCHER",25,340,360,62,launcher_cb,nullptr,0x9F1239);label(g_screen,"Hold BOOT 1.5 s anywhere for LauncherOS.",65,430,&lv_font_montserrat_12,0x94A3B8);
}

static void build_screen(ScreenID id){
  g_sleep_locks&=~LOCK_GAME;lv_obj_t *old=g_screen;g_screen=nullptr;g_header_time=nullptr;g_status=nullptr;
  switch(id){
    case SCREEN_HOME:build_home();break;case SCREEN_JOURNAL:build_journal();break;case SCREEN_RECORD:build_record();break;case SCREEN_TIMELINE:build_timeline();break;case SCREEN_DAILY:build_daily();break;
    case SCREEN_JOURNEY:build_journey();break;case SCREEN_COMPANION:build_companion();break;case SCREEN_QUESTS:build_quests();break;case SCREEN_CHINESE:build_chinese();break;case SCREEN_CHINESE_LESSON:build_chinese_lesson();break;case SCREEN_CHINESE_TRACE:build_chinese_trace();break;case SCREEN_CHINESE_EXERCISE:build_chinese_exercise();break;case SCREEN_CHINESE_DICTIONARY:build_chinese_dictionary();break;case SCREEN_CHINESE_PROGRESS:build_chinese_progress();break;
    case SCREEN_KNOWLEDGE:build_knowledge();break;case SCREEN_KNOWLEDGE_QUIZ:build_knowledge_quiz();break;case SCREEN_JOURNEY_HISTORY:build_journey_history();break;case SCREEN_BARGAIN:build_bargain();break;case SCREEN_CURRENCY:build_currency();break;case SCREEN_PHRASES:build_phrases();break;case SCREEN_LOCKER:build_locker();break;case SCREEN_PACKING:build_packing();break;
    case SCREEN_GAMES:build_games();break;case SCREEN_RPG:build_game(0,"TinyQuest","Explore");break;case SCREEN_SPACE:build_game(1,"Space Trader","Trade route");break;case SCREEN_KINGDOM:build_game(2,"Pocket Kingdom","Build");break;case SCREEN_CREATURES:build_game(3,"Creatures","Search");break;
    case SCREEN_HEALTH:build_health();break;case SCREEN_SETTINGS:build_settings();break;case SCREEN_TIME:build_time();break;case SCREEN_STORAGE:build_storage();break;case SCREEN_DIAGNOSTICS:build_diagnostics();break;case SCREEN_PASS:build_pass();break;case SCREEN_TORCH:build_torch();break;case SCREEN_EMERGENCY:build_emergency();break;case SCREEN_SAFE_MODE:build_safe_mode();break;default:build_home();break;
  }
  lv_scr_load_anim(g_screen,LV_SCR_LOAD_ANIM_FADE_ON,120,0,false);if(old&&old!=g_screen)lv_obj_del_async(old);g_last_touch_ms=millis();
}

// =============================================================================
// 13. LAUNCHER RETURN / OTA ACCEPTANCE
// =============================================================================
static void accept_running_app(){
  esp_ota_mark_app_valid_cancel_rollback();
  Serial.println("[boot] Voyager ready and OTA image accepted");
}
static void launcher_return_now(){
  if(g_returning)return;
  g_returning=true;
  if(g_recording)recording_stop();
  save_all();
  lv_obj_t *cover=lv_obj_create(lv_layer_top());
  lv_obj_set_size(cover,LCD_WIDTH,LCD_HEIGHT);lv_obj_align(cover,LV_ALIGN_CENTER,0,0);
  lv_obj_set_style_bg_color(cover,lv_color_hex(0x020617),0);lv_obj_set_style_bg_opa(cover,LV_OPA_COVER,0);lv_obj_set_style_border_width(cover,0,0);
  label(cover,"SAVING SESSION...",80,211,&lv_font_montserrat_22,0x67E8F9);
  label(cover,"RETURNING TO LAUNCHEROS",89,252,&lv_font_montserrat_14,0x94A3B8);
  lv_timer_handler();
  const esp_partition_t *factory=esp_partition_find_first(ESP_PARTITION_TYPE_APP,ESP_PARTITION_SUBTYPE_APP_FACTORY,nullptr);
  if(!factory||esp_ota_set_boot_partition(factory)!=ESP_OK){
    lv_obj_del(cover);g_returning=false;g_health.lastError=201;save_all();
    show_toast("Factory LauncherOS partition not found.\nNo alternate OTA slot was selected.");
    return;
  }
  fade_to(0,280);delay(80);esp_restart();
}
static void poll_boot_button(){
  static uint32_t held=0;
  if(g_boot_release){if(digitalRead(BOOT_BTN)==HIGH)g_boot_release=false;return;}
  if(digitalRead(BOOT_BTN)==LOW){
    if(g_screen_off){set_screen_off(false);held=millis();}
    else if(!held)held=millis();
    else if(millis()-held>=VOYAGER_BOOT_HOLD_MS)launcher_return_now();
  }else held=0;
}

// =============================================================================
// 14. BOOT / SETUP / LOOP
// =============================================================================
static void hardware_init(){
  Serial.begin(115200);pinMode(BOOT_BTN,INPUT_PULLUP);pinMode(PWR_BTN_PIN,INPUT_PULLUP);pinMode(TP_INT,INPUT_PULLUP);btStop();
  g_boot_release=digitalRead(BOOT_BTN)==LOW;
  if(!g_gfx->begin())Serial.println("[gfx] begin failed");
  g_gfx->fillScreen(RGB565_BLACK);static_cast<Arduino_CO5300*>(g_gfx)->setBrightness(0);g_current_brightness=0;
  pinMode(IIC_SCL,OUTPUT);
  for(int i=0;i<9;i++){digitalWrite(IIC_SCL,LOW);delayMicroseconds(5);digitalWrite(IIC_SCL,HIGH);delayMicroseconds(5);}
  Wire.begin(IIC_SDA,IIC_SCL);Wire.setClock(400000);
  pinMode(TP_RESET,OUTPUT);digitalWrite(TP_RESET,LOW);delay(50);digitalWrite(TP_RESET,HIGH);delay(200);
  bool touch_ready=false;for(int i=0;i<3&&!touch_ready;i++){touch_ready=g_touch->begin();if(!touch_ready)delay(80);}
  if(touch_ready)g_touch->IIC_Write_Device_State(g_touch->Arduino_IIC_Touch::Device::TOUCH_POWER_MODE,g_touch->Arduino_IIC_Touch::Device_Mode::TOUCH_POWER_MONITOR);else Serial.println("[touch] FT3168 unavailable");
  pmu_init();imu_init();
  lv_init();size_t px=LCD_WIDTH*60;g_lvbuf=(lv_color_t*)heap_caps_malloc(px*sizeof(lv_color_t),MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);if(!g_lvbuf)g_lvbuf=(lv_color_t*)heap_caps_malloc(px*sizeof(lv_color_t),MALLOC_CAP_SPIRAM);if(!g_lvbuf)g_lvbuf=(lv_color_t*)heap_caps_malloc(px*sizeof(lv_color_t),MALLOC_CAP_8BIT);if(!g_lvbuf){Serial.println("[lvgl] fatal draw buffer allocation");while(true)delay(1000);}
  lv_disp_draw_buf_init(&g_draw_buf,g_lvbuf,nullptr,px);lv_disp_drv_init(&g_disp_drv);g_disp_drv.flush_cb=flush_cb;g_disp_drv.draw_buf=&g_draw_buf;g_disp_drv.hor_res=LCD_WIDTH;g_disp_drv.ver_res=LCD_HEIGHT;lv_disp_drv_register(&g_disp_drv);
  lv_indev_drv_init(&g_indev_drv);g_indev_drv.type=LV_INDEV_TYPE_POINTER;g_indev_drv.read_cb=touch_cb;g_indev=lv_indev_drv_register(&g_indev_drv);
}
static void boot_splash(){
  lv_obj_t *s=lv_obj_create(NULL);lv_obj_set_style_bg_color(s,lv_color_hex(0x020617),0);lv_obj_clear_flag(s,LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *dot=lv_obj_create(s);lv_obj_set_size(dot,26,26);lv_obj_align(dot,LV_ALIGN_CENTER,0,-70);lv_obj_set_style_radius(dot,LV_RADIUS_CIRCLE,0);lv_obj_set_style_bg_color(dot,lv_color_hex(0x67E8F9),0);lv_obj_set_style_border_width(dot,0,0);
  label(s,"VOYAGER//OS",100,235,&lv_font_montserrat_28,0xF8FAFC);
  label(s,"F U S I O N   W A T C H",82,278,&lv_font_montserrat_14,0x67E8F9);
  label(s,"DISPLAY  TOUCH  STORAGE  SENSORS",73,427,&lv_font_montserrat_12,0x94A3B8);
  lv_scr_load(s);lv_timer_handler();fade_to(VOYAGER_DEFAULT_BRIGHTNESS,260);g_screen=s;
}
void setup(){
  esp_sleep_get_wakeup_cause();hardware_init();boot_splash();delay(350);
  rtc_init();storage_init();audio_init();load_all();recover_recording();
  g_brightness=constrain(g_brightness,20,255);fade_to(g_brightness,120);
  if(g_pmu_ok){g_battery=g_pmu.getBatteryPercent();g_charging=g_pmu.isCharging();}
  build_screen(SCREEN_HOME);lv_timer_handler();accept_running_app();g_last_touch_ms=g_last_wake_ms=millis();
#if VOYAGER_POWER_SAVE
  setCpuFrequencyMhz(80);
#endif
}
void loop(){
  lv_timer_handler();service_audio();service_imu();service_power();service_header();poll_boot_button();uint32_t now=millis();
  if(g_pmu_ok&&now-g_last_battery_ms>10000){g_last_battery_ms=now;g_battery=g_pmu.getBatteryPercent();g_charging=g_pmu.isCharging();}
  if(now-g_last_save_ms>300000UL)save_all();
  if(g_sd_ok&&free_sd()<VOYAGER_CRITICAL_FREE_BYTES&&!g_recording){g_health.lastError=301;}
  delay(g_recording?1:5);
}
