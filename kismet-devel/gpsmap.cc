/*
    This file is part of Kismet

    Kismet is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    Kismet is distributed in the hope that it will be useful,
      but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Kismet; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

// Extract a GPS coordinates dump and list it

// Map fetcher
// Map: http://www.mapblast.com/gif?&CT=41.711632:-73.932541:25000&IC=&W=1280&H=1024&FAM=mblast&LB=
// lat, lon, zoom

#include "config.h"

// Prevent make dep warnings
#if (defined(HAVE_IMAGEMAGICK) && defined(HAVE_GPS))

#include <stdio.h>
#ifdef HAVE_PTHREAD
#include <pthread.h>
#endif
#ifdef HAVE_STDINT_H
#include <stdint.h>
#endif
#ifdef HAVE_INTTYPES_H
#include <inttypes.h>
#endif
#include <math.h>
#include <time.h>
#include "getopt.h"
#include <unistd.h>
#include <list>
#include <map>
#include <vector>
#include <algorithm>
#include <string>
#include <deque>
#include <algorithm>
#include <zlib.h>
#include <magick/api.h>
#include "configfile.h"

#include "gpsdump.h"
#include "expat.h"
#include "manuf.h"

/* Mapscale / pixelfact is meter / pixel */
#define PIXELFACT 2817.947378

#define square(x) ((x)*(x))
// Global constant values
const char *config_base = "kismet.conf";
// &L = USA for the USA, EUR appears to be generic for Europe, EUR0809 for other parts of Europe.. if you get it wrong your map will be very bland =)
// default to USA, probably want to change this. -- poptix
const char url_template_mp[] = "http://msrvmaps.mappoint.net/isapi/MSMap.dll?ID=3XNsF.&C=%f,%f&L=USA&CV=1&A=%ld&S=%d,%d&O=0.000000,0.000000&MS=0&P=|5748|";
const char url_template_ts[] = "http://terraservice.net/GetImageArea.ashx?t=1&lat=%f&lon=%f&s=%ld&w=%d&h=%d";
const char url_template_mb[] = "http://www.vicinity.com/gif?&CT=%f:%f:%ld&IC=&W=%d&H=%d&FAM=myblast&LB=%s";
// Freaking mapblast changed again...
// const char url_template_mb[] = "http://www.mapblast.com/myblastd/MakeMap.d?&CT=%f:%f:%ld&IC=&W=%d&H=%d&FAM=myblast&LB=%s";
const char url_template_ti[] = "http://tiger.census.gov/cgi-bin/mapper/map.gif?lat=%f&lon=%f&wid=0.001&ht=%f&iwd=%d&iht=%d&on=majroads&on=places&on=shorelin&on=streets&on=interstate&on=statehwy&on=ushwy&on=water&tlevel=-&tvar=-&tmeth=i";

const char download_template[] = "wget \"%s\" -O %s";
// Decay from absolute blue for multiple tracks
const uint8_t track_decay = 0x1F;
// distance (in feet) before we throttle a network and discard it
const unsigned int horiz_throttle = 75000;

// Image scales we use
long int scales[] = { 1000, 2000, 5000, 10000, 20000, 30000, 50000, 60000, 70000, 75000, 80000,
85000, 90000, 95000, 100000, 125000, 150000, 200000, 300000, 500000, 750000, 1000000, 2000000,
3000000, 4000000, 5000000, 6000000, 7000000, 8000000, 9000000, 10000000, 15000000,
20000000, 25000000, 30000000, 35000000, 40000000, 0 };
// Terraserver scales for conversion to mapblast scales
long int terrascales[] = { 2757, 5512, 11024, 22048, 44096, 88182, 176384 };

// Image colors
const char *netcolors[] = {
    "#000000",
    "#FF0000", "#FF0072", "#FF00E5", "#D400FF",
    "#5D00FF", "#0087FF", "#00F2FF", "#00FF94",
    "#00FF2E", "#BBFF00", "#FFB200", "#FF6E00",
    "#FF6500", "#960000", "#96005F", "#640096",
    "#001E96", "#008E96", "#00963E", "#529600",
    "#968C00", "#963700", NULL
};

// Channel colors
char *channelcolors[] = {
    "#FF0000", "#FF8000", "#FFFF00",
    "#80FF00", "#00FF00", "#00FF80",
    "#00FFFF", "#0080FF", "#0000FF",
    "#8000FF", "#FF00FF", "#FF0080",
    "#808080", "#CCCCCC"
};
int channelcolor_max = 14;

// Origional
char *powercolors_Orig[] = {
    "#FF0000", "#FFD500", "#FFCC00",
    "#F2FF00", "#7BFF00", "#00FFB6",
    "#00FFFF", "#005DFF", "#A100FF",
    "#FA00FF"
};
const int power_steps_Orig = 10;
// Blue powercolors
char *powercolors_Blue[] = {
    "#A0A0FF",
    "#9B9BFA",
    "#9696F5",
    "#9191F0",
    "#8C8CEB",
    "#8787E6",
    "#8282E1",
    "#7D7DDC",
    "#7878D7",
    "#7373D2",
    "#6E6ECD",
    "#6969C8",
    "#6464C3",
    "#5F5FBE",
    "#5A5AB9",
    "#5555B4",
    "#5050AF",
    "#4B4BAA",
    "#4646A5",
    "#4141A0",
    "#3C3C9B",
    "#373796",
    "#323291",
    "#2D2D8C",
    "#282887",
    "#232382",
    "#1E1E7D",
    "#191978",
    "#141473",
    "#0F0F6E",
    "#0A0A69",
    "#050564",
};
const int power_steps_Blue = 32;

// Math progression
char *powercolors_Math[] = {
    "#FF0000", "#FF8000", "#FFFF00",
    "#80FF00", "#00FF00", "#00FF80",
    "#00FFFF", "#0080FF", "#0000FF",
    "#8000FF", "#FF00FF", "#FF0080"
};
const int power_steps_Math = 12;
// Weather Radar
char *powercolors_Radar[] = {
    "#50E350", "#39C339", "#208420",
    "#145A14", "#C8C832", "#DC961E",
    "#811610", "#B31A17", "#E61E1E"
};
const int power_steps_Radar = 9;
// Maximum power reported
const int power_max = 255;

int powercolor_index = 0;

// Label gravity
char *label_gravity_list[] = {
    "northwest", "north", "northeast",
    "west", "center", "east",
    "southwest", "south", "southeast"
};

int scatter_power;
int power_zoom;

// Tracker internals
int sample_points;

// Convex hull point
struct hullPoint {
	int x, y;
	double angle;
	string xy;
	bool operator< (const hullPoint&) const;
	bool operator() (const hullPoint&, const hullPoint&) const;
};

bool hullPoint::operator< (const hullPoint& op) const {
	if (y == op.y) {
		return x < op.x;
	}
	return y < op.y;
}

bool hullPoint::operator() (const hullPoint& a, const hullPoint& b) const {
	if (a.angle == b.angle) {
		if (a.x == b.x) {
			return a.y < b.y;
		}
		return a.x < b.x;
	}
	return a.angle < b.angle;
}

typedef struct gps_network {

    gps_network() {
        filtered = 0;
        wnet = NULL;
        max_lat = 90;
        max_lon = 180;
        min_lat = -90;
        min_lon = -180;
        max_alt = min_alt = 0;
        count = 0;
        avg_lat = avg_lon = avg_alt = avg_spd = 0;
        diagonal_distance = altitude_distance = 0;
    };

    // Are we filtered from displying?
    int filtered;

    // Wireless network w/ full details, loaded from the associated netfile xml
    wireless_network *wnet;

    string bssid;

    float max_lat;
    float min_lat;
    float max_lon;
    float min_lon;
    float max_alt;
    float min_alt;

    int count;

    float avg_lat, avg_lon, avg_alt, avg_spd;

    float diagonal_distance, altitude_distance;

    vector<gps_point *> points;

    // Points w/in this network
    // vector<point> net_point;

    // Index to the netcolors table
    string color_index;

    struct {
	int x, y, h, w; } label;
};

// All networks we know about
map<mac_addr, wireless_network *> bssid_net_map;

// All the networks we know about for drawing
map<string, gps_network *> bssid_gpsnet_map;
// All networks we're going to draw
map<mac_addr, gps_network *> drawn_net_map;

typedef struct {
    int version;

    float lat;
    float lon;
    float alt;
    float spd;

    int power;
    int quality;
    int noise;

    unsigned int x, y;
} track_data;

// Array of network track arrays
unsigned int num_tracks = 0;
vector< vector<track_data> > track_vec;
// Global average for map scaling
gps_network global_map_avg;

// Do we have any power data?
int power_data;

// Map scape
long map_scale;
// Center lat/lon for map
double map_avg_lat, map_avg_lon;


// User options and defaults
unsigned int map_width = 1280;
unsigned int map_height = 1024;
const int legend_height = 100;

// Drawing features and opacity
int draw_track = 0, draw_bounds = 0, draw_range = 0, draw_power = 0,
    draw_hull = 0, draw_scatter = 0, draw_legend = 0, draw_center = 0, draw_label = 0;
int track_opacity = 100, /* no bounds opacity */ range_opacity = 70, power_opacity = 70,
    hull_opacity = 70, scatter_opacity = 100, center_opacity = 100, label_opacity = 100;
int track_width = 3;
int convert_greyscale = 1, keep_gif = 0, verbose = 0, label_orientation = 7;

// Offsets for drawing from what we calculated
int draw_x_offset = 0, draw_y_offset = 0;

// Map source
int mapsource = 0;
#define MAPSOURCE_MAPBLAST   0
#define MAPSOURCE_MAPPOINT   1
#define MAPSOURCE_TERRA      2
#define MAPSOURCE_TIGER      3

// Interpolation resolution
int power_resolution = 5;
// Interpolation colors
// strength colors
char **power_colors = NULL;
int power_steps = 0;
// Center resolution (size of circle)
int center_resolution = 2;
// Scatter resolution
int scatter_resolution = 2;
// Labels to draw
string network_labels;
// Order to draw in
string draw_feature_order = "ptbrhscl";
// Color coding (1 = wep, 2 = channel)
int color_coding = 0;
#define COLORCODE_NONE    0
#define COLORCODE_WEP     1
#define COLORCODE_CHANNEL 2

// Threads, locks, and graphs to hold the power
#ifdef HAVE_PTHREAD
pthread_t *mapthread;
pthread_mutex_t power_lock;
pthread_mutex_t print_lock;
pthread_mutex_t power_pos_lock;
#endif
unsigned int numthreads = 1;
unsigned int *power_map;
unsigned int power_pos = 0;
int *power_input_map;

// AP and client maps
macmap<vector<manuf *> > ap_manuf_map;
macmap<vector<manuf *> > client_manuf_map;

// Filtered MAC's
int invert_filter = 0;
macmap<int> filter_map;

// Filtered types
map<wireless_network_type, int> type_filter_map;
int invert_type_filter;

// Exception/error catching
ExceptionInfo im_exception;

// Signal levels
int signal_lowest = 255;
int signal_highest = -255;

// Forward prototypes
string Mac2String(uint8_t *mac, char seperator);
string NetType2String(wireless_network_type in_type);
void UpdateGlobalCoords(float in_lat, float in_lon, float in_alt);
double rad2deg(double x);
double earth_distance(double lat1, double lon1, double lat2, double lon2);
double calcR (double lat);
void calcxy (double *posx, double *posy, double lat, double lon, double pixelfact, /*FOLD00*/
             double zero_lat, double zero_long);
long int BestMapScale(double tlat, double tlon, double blat, double blon);
int ProcessGPSFile(char *in_fname);
void AssignNetColors();
void MergeNetData(vector<wireless_network *> in_netdata);
void ProcessNetData(int in_printstats);
void DrawNetTracks(vector< vector<track_data> > in_tracks, Image *in_img, DrawInfo *in_di);
void DrawNetCircles(vector<gps_network *> in_nets, Image *in_img, DrawInfo *in_di);
void DrawNetBoundRects(vector<gps_network *> in_nets, Image *in_img, DrawInfo *in_di, int in_fill);
void DrawNetCenterDot(vector<gps_network *> in_nets, Image *in_img, DrawInfo *in_di);
int InverseWeight(int in_x, int in_y, int in_fuzz, double in_scale);
void DrawNetPower(vector<gps_network *> in_nets, Image *in_img, DrawInfo *in_di);
void DrawNetHull(vector<gps_network *> in_nets, Image *in_img, DrawInfo *in_di);
void DrawNetScatterPlot(vector<gps_network *> in_nets, Image *in_img, DrawInfo *in_di);

int DrawLegendComposite(vector<gps_network *> in_nets, Image **in_img, DrawInfo **in_di);

int IMStringWidth(const char *psztext, Image *in_img, DrawInfo *in_di);
int IMStringHeight(const char *psztext, Image *in_img, DrawInfo *in_di);

// Algo sort by lon
class PointSortLon {
public:
    inline bool operator() (const gps_point *x, const gps_point *y) const {
        if (isnan(x->lon))
            return 1;

        if (isnan(y->lon))
            return 0;

        if (x->lon < y->lon)
            return 1;
        return 0;
    }
};

// Algo sort by lon
class PointSortLat {
public:
    inline bool operator() (const gps_point *x, const gps_point *y) const {
        if (isnan(x->lat))
            return 1;

        if (isnan(y->lon))
            return 0;

        if (x->lat < y->lat)
            return 1;
        return 0;
    }
};

int IMStringWidth(const char *psztext, Image *in_img, DrawInfo *in_di) {
    ExceptionInfo ex;
    TypeMetric metrics;

    in_di->text = (char *) psztext;
    GetExceptionInfo(&ex);
    if (!GetTypeMetrics(in_img, in_di, &metrics)) {
        GetImageException(in_img, &ex);
        fprintf(stderr, "stringheight.. %s %s\n", ex.reason, ex.description);
        return 0;
    }

    in_di->text = NULL;

    return (int) fabs(metrics.width);
}

int IMStringHeight(const char *psztext, Image *in_img, DrawInfo *in_di) {
    ExceptionInfo ex;
    TypeMetric metrics;

    in_di->text = (char *) psztext;
    GetExceptionInfo(&ex);
    if (!GetTypeMetrics(in_img, in_di, &metrics)) {
        GetImageException(in_img, &ex);
        fprintf(stderr, "stringwidth... %s %s\n", ex.reason, ex.description);
        return 0;
    }

    in_di->text = NULL;

    return (int) fabs(metrics.height);
}

string Mac2String(uint8_t *mac, char seperator) { /*FOLD00*/
    char tempstr[MAC_STR_LEN];

    // There must be a better way to do this...
    if (seperator != '\0')
        snprintf(tempstr, MAC_STR_LEN, "%02X%c%02X%c%02X%c%02X%c%02X%c%02X",
                 mac[0], seperator, mac[1], seperator, mac[2], seperator,
                 mac[3], seperator, mac[4], seperator, mac[5]);
    else
        snprintf(tempstr, MAC_STR_LEN, "%02X%02X%02X%02X%02X%02X",
                 mac[0], mac[1], mac[2],
                 mac[3], mac[4], mac[5]);

    string temp = tempstr;
    return temp;
}

string NetType2String(wireless_network_type in_type) {
    if (in_type == network_ap)
        return "ap";
    if (in_type == network_adhoc)
        return "adhoc";
    if (in_type == network_probe)
        return "probe";
    if (in_type == network_turbocell)
        return "turbocell";
    if (in_type == network_data)
        return "data";

    return "unknown";
}

void UpdateGlobalCoords(float in_lat, float in_lon, float in_alt) {
    if (in_lat > global_map_avg.max_lat || global_map_avg.max_lat == 90)
        global_map_avg.max_lat = in_lat;
    if (in_lat < global_map_avg.min_lat || global_map_avg.min_lat == -90)
        global_map_avg.min_lat = in_lat;

    if (in_lon > global_map_avg.max_lon || global_map_avg.max_lon == 180)
        global_map_avg.max_lon = in_lon;
    if (in_lon < global_map_avg.min_lon || global_map_avg.min_lon == -180)
        global_map_avg.min_lon = in_lon;

    if (in_alt > global_map_avg.max_alt || global_map_avg.max_alt == 0)
        global_map_avg.max_alt = in_alt;
    if (in_alt < global_map_avg.min_alt || global_map_avg.min_alt == 0)
        global_map_avg.min_alt = in_alt;

}

void SanitizeSamplePoints(vector<gps_point *> in_samples, map<int,int> *dead_sample_ids) {
    dead_sample_ids->clear();

    // Two copies of our sample vector... yeah, this eats ram.  So does the whole app.
    vector<gps_point *> lat_samples = in_samples;
    vector<gps_point *> lon_samples = in_samples;

    // Clean up offset-valid (in broken, limited capture files) points
    for (unsigned int pos = 0; pos < in_samples.size(); pos++) {
        if ((in_samples[pos]->lat == 0 && in_samples[pos]->lon == 0) ||
            (isnan(in_samples[pos]->lat) || isinf(in_samples[pos]->lat) ||
             isnan(in_samples[pos]->lon) || isinf(in_samples[pos]->lon)))
            (*dead_sample_ids)[in_samples[pos]->id] = 1;
    }

    // Bail out on small lists that we won't be able to get anything out of
    if (in_samples.size() < 4)
        return;

    // Sort the networks
    stable_sort(lat_samples.begin(), lat_samples.end(), PointSortLat());
    stable_sort(lon_samples.begin(), lon_samples.end(), PointSortLon());

    // Lets make the assumption that half our sample points can't be crap....
    int slice_point = -1;
    for (unsigned int pos = 0; pos < (lat_samples.size() / 2) + 1; pos++) {
        float lat_offset = lat_samples[pos + 1]->lat - lat_samples[pos]->lat;

        // Slice if we have a major break, it can only get worse from here...
        if (lat_offset > 0.5 || lat_offset < -0.5) {
            /*
              printf("Major lat break at pos %d in sorted lats, %f,%f id %d\n",
                pos, lat_samples[pos]->lat, lat_samples[pos]->lon, lat_samples[pos]->id);
            */
            slice_point = pos;
            break;
        }
    }

    if (slice_point != -1) {
        for (unsigned int pos = 0; pos <= (unsigned int) slice_point; pos++) {
            //printf("Discarding point lat violation %f,%f %d...\n", lat_samples[pos]->lat, lat_samples[pos]->lon, lat_samples[pos]->id);
            (*dead_sample_ids)[lat_samples[pos]->id] = 1;
        }
    }

    // Now for the upper bounds of the lat...
    slice_point = -1;
    for (unsigned int pos = (lat_samples.size() / 2) - 1; pos < lat_samples.size(); pos++) {
        float lat_offset = lat_samples[pos - 1]->lat - lat_samples[pos]->lat;

        // Slice if we have a major break, it can only get worse from here...
        if (lat_offset > 0.5 || lat_offset < -0.5) {
            /*
             printf("Major lat break at pos %d in sorted lats, %f,%f id %d\n",
             pos, lat_samples[pos]->lat, lat_samples[pos]->lon, lat_samples[pos]->id);
             */
            slice_point = pos;
            break;
        }
    }

    if (slice_point != -1) {
        for (unsigned int pos = slice_point; pos < lat_samples.size(); pos++) {
            //printf("Discarding point lat violation %f,%f %d...\n", lat_samples[pos]->lat, lat_samples[pos]->lon, lat_samples[pos]->id);
            (*dead_sample_ids)[lat_samples[pos]->id] = 1;
        }
    }


    // Now for the lon...
    slice_point = -1;
    for (unsigned int pos = 0; pos < (lon_samples.size() / 2) + 1; pos++) {
        float lon_offset = lon_samples[pos + 1]->lon - lon_samples[pos]->lon;

        // Slice if we have a major break, it can only get worse from here...
        if (lon_offset > 0.5 || lon_offset < -0.5) {
            /*
            printf("Major lon break at pos %d in sorted lons, %f,%f id %d\n",
            pos, lon_samples[pos]->lon, lon_samples[pos]->lon, lon_samples[pos]->id);
            */
            slice_point = pos;
            break;
        }
    }

    if (slice_point != -1) {
        for (unsigned int pos = 0; pos <= (unsigned int) slice_point; pos++) {
            // printf("Discarding point lon violation %f,%f %d...\n", lon_samples[pos]->lon, lon_samples[pos]->lon, lon_samples[pos]->id);
            (*dead_sample_ids)[lon_samples[pos]->id] = 1;
        }
    }

    // Now for the lon upper bound...
    slice_point = -1;
    for (unsigned int pos = lon_samples.size() / 2; pos < lon_samples.size(); pos++) {
        float lon_offset = lon_samples[pos - 1]->lon - lon_samples[pos]->lon;

        // Slice if we have a major break, it can only get worse from here...
        if (lon_offset > 0.5 || lon_offset < -0.5) {
            /*
            printf("Major lon break at pos %d in sorted lons, %f,%f id %d\n",
            pos, lon_samples[pos]->lon, lon_samples[pos]->lon, lon_samples[pos]->id);
            */
            slice_point = pos;
            break;
        }
    }

    if (slice_point != -1) {
        for (unsigned int pos = slice_point; pos < lon_samples.size(); pos++) {
            // printf("Discarding point lon violation %f,%f %d...\n", lon_samples[pos]->lon, lon_samples[pos]->lon, lon_samples[pos]->id);
            (*dead_sample_ids)[lon_samples[pos]->id] = 1;
        }
    }

}

void MergeNetData(vector<wireless_network *> in_netdata) {
    for (unsigned int x = 0; x < in_netdata.size(); x++) {
        wireless_network *inet = in_netdata[x];

        map<mac_addr, wireless_network *>::iterator bnmi = bssid_net_map.find(inet->bssid);
        if (bnmi != bssid_net_map.end()) {
            wireless_network *onet = bnmi->second;

            // Update stuff if it's info we don't have, period

            if (onet->type > inet->type)
                onet->type = inet->type;

            if (onet->ssid == "")
                onet->ssid = inet->ssid;

            if (onet->channel == 0)
                onet->channel = inet->channel;

            if (onet->beacon_info == "")
                onet->beacon_info = inet->beacon_info;

            if (onet->ipdata.atype < inet->ipdata.atype)
                memcpy(&onet->ipdata, &inet->ipdata, sizeof(net_ip_data));

            if (onet->last_time < inet->last_time) {
                // Update stuff if it's better in the newer data.  This may cause a
                // double-update but this only happens once in a massively CPU intensive
                // utility so I don't care.
                onet->last_time = inet->last_time;

                if (inet->ssid != "")
                    onet->ssid = inet->ssid;

                if (inet->channel != 0)
                    onet->channel = inet->channel;

                if (inet->beacon_info != "")
                    onet->beacon_info = inet->beacon_info;

                onet->cloaked = inet->cloaked;
                onet->wep = inet->wep;
            }

            if (onet->first_time < inet->first_time)
                onet->first_time = inet->first_time;

            onet->llc_packets += inet->llc_packets;
            onet->data_packets += inet->data_packets;
            onet->crypt_packets += inet->crypt_packets;
            onet->interesting_packets += inet->interesting_packets;
        } else {
            bssid_net_map[inet->bssid] = inet;
        }
    }
}

int ProcessGPSFile(char *in_fname) {
    int file_samples = 0;

#ifdef HAVE_LIBZ
    gzFile gpsfz;
#else
    FILE *gpsf;
#endif

#ifdef HAVE_LIBZ
    if ((gpsfz = gzopen(in_fname, "rb")) == NULL) {
        fprintf(stderr, "FATAL:  Could not open data file\n");
        return -1;
    }
#else
    if ((gpsf = fopen(in_fname, "r")) == NULL) {
        fprintf(stderr, "FATAL:  Could not open data file.\n");
        return -1;
    }
#endif

    fprintf(stderr, "NOTICE:  Processing gps file '%s'\n", in_fname);

    vector<gps_point *> file_points;
    map<int, int> file_screen;
#ifdef HAVE_LIBZ
    file_points = XMLFetchGpsList(gpsfz);
#else
    file_points = XMLFetchGpsList(gpsf);
#endif
    file_samples = file_points.size();

    if (file_samples == 0) {
        fprintf(stderr, "WARNING:  No sample points found in '%s'.\n", in_fname);
        return 0;
    }

    // We handle the points themselves after we handle the network component

#ifdef HAVE_LIBZ
    gzclose(gpsfz);
#else
    fclose(gpsf);
#endif

    // We have the file correctly, so add to our gps track count
    vector<track_data> trak;
    track_vec.push_back(trak);
    num_tracks++;

    // We have all our gps points loaded into the local struct now, so if they had a
    // network file specified, load the networks from that and mesh it with the network
    // data we already (may) have from ther files.

    int foundnetfile = 0;
    string comp;

    if ((comp = XMLFetchGpsNetfile()) != "") {
        if (verbose)
            fprintf(stderr, "NOTICE:  Reading associated network file, '%s'\n", 
                    XMLFetchGpsNetfile().c_str());
#ifdef HAVE_LIBZ
        if ((gpsfz = gzopen(XMLFetchGpsNetfile().c_str(), "r")) == NULL) {
            if (verbose)
                fprintf(stderr, "WARNING:  Could not open associated network "
                        "xml file '%s'.\n", XMLFetchGpsNetfile().c_str());
        } else {
            foundnetfile = 1;
        }

        // Try our alternate file methods

        if (foundnetfile == 0) {
            comp = XMLFetchGpsNetfile();
            comp += ".gz";

            if ((gpsfz = gzopen(comp.c_str(), "r")) == NULL) {
                if (verbose)
                    fprintf(stderr, "WARNING:  Could not open compressed network "
                            "xml file '%s'\n", comp.c_str());
            } else {
                foundnetfile = 1;
            }
        }

        if (foundnetfile == 0) {
            string orignetfile = XMLFetchGpsNetfile();
            string origxmlfile = in_fname;

            // Prepend a ./ to the files if it isn't there
            if (origxmlfile[0] != '/' && origxmlfile[0] != '.')
                origxmlfile = "./" + origxmlfile;
            if (orignetfile[0] != '/' && orignetfile[0] != '.')
                orignetfile = "./" + orignetfile;

            // Break up the path to the gpsxml file and form a path based on that
            unsigned int lastslash = 0;
            for (unsigned int x = origxmlfile.find('/'); x != string::npos;
                 lastslash = x, x = origxmlfile.find('/', lastslash+1)) {
                // We don't actually need to do anything...
            }

            comp = origxmlfile.substr(0, lastslash);

            lastslash = 0;
            for (unsigned int x = orignetfile.find('/'); x != string::npos;
                 lastslash = x, x = orignetfile.find('/', lastslash+1)) {
                // We don't actually need to do anything...
            }

            comp += "/" + orignetfile.substr(lastslash, orignetfile.size() - lastslash);

            if (comp != origxmlfile) {
                if ((gpsfz = gzopen(comp.c_str(), "r")) == NULL) {
                    if (verbose)
                        fprintf(stderr, "WARNING:  Could not open network xml file "
                                "relocated to %s\n", comp.c_str());
                } else {
                    foundnetfile = 1;
                }

                // And look again for our relocated compressed file.
                if (foundnetfile == 0) {
                    comp += ".gz";
                    if ((gpsfz = gzopen(comp.c_str(), "r")) == NULL) {
                        if (verbose)
                            fprintf(stderr, "WARNING:  Could not open compressed "
                                    "network xml file relocated to %s\n",
                                    comp.c_str());
                    } else {
                        foundnetfile = 1;
                    }
                }
            }
        }

#else
        if ((gpsf = fopen(XMLFetchGpsNetfile().c_str(), "r")) == NULL) {
            if (verbose)
                fprintf(stderr, "WARNING:  Could not open associated network "
                        "xml file '%s'\n", XMLFetchGpsNetfile().c_str());
        } else {
            foundnetfile = 1;
        }

        // Try our alternate file methods

        if (foundnetfile == 0) {
            string orignetfile = XMLFetchGpsNetfile();
            string origxmlfile = in_fname;

            // Prepend a ./ to the files if it isn't there
            if (origxmlfile[0] != '/' && origxmlfile[0] != '.')
                origxmlfile = "./" + origxmlfile;
            if (orignetfile[0] != '/' && orignetfile[0] != '.')
                orignetfile = "./" + orignetfile;

            // Break up the path to the gpsxml file and form a path based on that
            unsigned int lastslash = 0;
            for (unsigned int x = origxmlfile.find('/'); x != string::npos;
                 lastslash = x, x = origxmlfile.find('/', lastslash+1)) {
                // We don't actually need to do anything...
            }

            comp = origxmlfile.substr(0, lastslash);

            lastslash = 0;
            for (unsigned int x = orignetfile.find('/'); x != string::npos;
                 lastslash = x, x = orignetfile.find('/', lastslash+1)) {
                // We don't actually need to do anything...
            }

            comp += "/" + orignetfile.substr(lastslash, orignetfile.size() - lastslash - 1);

            if (comp != origxmlfile) {
                if ((gpsf = fopen(comp.c_str(), "r")) == NULL) {
                    if (verbose)
                        fprintf(stderr, "WARNING:  Could not open network xml file "
                                "relocated to %s\n", comp.c_str());
                } else {
                    foundnetfile = 1;
                }

            }
        }

#endif

        if (foundnetfile) {
            fprintf(stderr, "NOTICE:  Opened associated network xml file '%s'\n", 
                    comp.c_str());

            if (verbose)
                fprintf(stderr, "NOTICE:  Processing network XML file.\n");

            vector<wireless_network *> file_networks;
#ifdef HAVE_LIBZ
            file_networks = XMLFetchNetworkList(gpsfz);
#else
            file_networks = XMLFetchNetworkList(gpsf);
#endif
            if (file_networks.size() != 0) {
                // Do something with the networks
                MergeNetData(file_networks);
            } else {
                fprintf(stderr, "WARNING:  No network entries found in '%s'.\n",
                        XMLFetchGpsNetfile().c_str());
            }
#ifdef HAVE_LIBZ
            gzclose(gpsfz);
#else
            fclose(gpsf);
#endif
        }
    }

    // Now that we have the network data (hopefully) loaded, we'll load the points and
    // reference the networks for them.
    int last_power = 0;
    int power_count = 0;

    // Bail if we don't have enough samples to make it worth it.
    if (file_points.size() < 50) {
        fprintf(stderr, "WARNING:  Skipping file '%s', too few sample points to get "
                "valid data.\n", in_fname);
        return 1;
    }
    
    // Sanitize the data and build the map of points we don't look at
    if (verbose)
        fprintf(stderr, "NOTICE:  Sanitizing %d sample points...\n", 
                file_points.size());
    SanitizeSamplePoints(file_points, &file_screen);

    for (unsigned int i = 0; i < file_points.size(); i++) {
        if (file_screen.find(file_points[i]->id) != file_screen.end()) {
            if (verbose)
                fprintf(stderr, "Removing invalid point %f,%f id %d from "
                        "consideration...\n", file_points[i]->lat, 
                        file_points[i]->lon, file_points[i]->id);
            continue;
        }

        // All we have to do here is push the points into the network (and make them
        // one if it doesn't exist).  We crunch all the data points in ProcessNetData
        gps_network *gnet = NULL;

            /*
        if (filter.length() != 0)
            if (((invert_filter == 0 && filter.find(file_points[i]->bssid) != string::npos) ||
                 (invert_filter == 1 && filter.find(file_points[i]->bssid) == string::npos)) &&
                strncmp(file_points[i]->bssid, gps_track_bssid, MAC_STR_LEN) != 0) {
                */

        // Don't process filtered macs at all.
        //macmap<int>::iterator fitr = 
        //    filter_map.find(mac_addr(file_points[i]->bssid));
        if ((invert_filter == 0 && 
             filter_map.find(file_points[i]->bssid) != filter_map.end()) ||
            (invert_filter == 1 && 
             filter_map.find(file_points[i]->bssid) != filter_map.end())) {
            continue;
        }

        // Don't process unfixed points at all
        if (file_points[i]->fix < 2)
            continue;

        double lat, lon, alt, spd;
        int fix;

        lat = file_points[i]->lat;
        lon = file_points[i]->lon;
        alt = file_points[i]->alt;
        spd = file_points[i]->spd;
        fix = file_points[i]->fix;

        // Only include tracks in the size of the map if we're going to draw them
        int trackdata = strncmp(file_points[i]->bssid, gps_track_bssid, MAC_STR_LEN);
        if ((draw_track && trackdata == 0) || trackdata != 0) {
            global_map_avg.avg_lon += lon;
            global_map_avg.avg_alt += alt;
            global_map_avg.avg_spd += spd;
            global_map_avg.count++;

            UpdateGlobalCoords(lat, lon, alt);
        }

        if (trackdata == 0) {
            track_data tdat;

            tdat.x = 0;
            tdat.y = 0;

            tdat.lat = lat;
            tdat.lon = lon;
            tdat.alt = alt;
            tdat.spd = spd;

            tdat.version = (int) XMLFetchGpsVersion();

            // Filter power ratings
            if (file_points[i]->signal == last_power) {
                if (power_count < 3) {
                    tdat.power = file_points[i]->signal;
                    tdat.quality = file_points[i]->quality;
                    tdat.noise = file_points[i]->noise;
                } else {
                    tdat.power = 0;
                    tdat.quality = 0;
                    tdat.noise = 0;
                }
                power_count++;
            } else {
                last_power = file_points[i]->signal;
                power_count = 0;
                tdat.power = file_points[i]->signal;
                tdat.quality = file_points[i]->quality;
                tdat.noise = file_points[i]->noise;
            }

            if (tdat.power != 0)
                power_data = 1;
            track_vec[num_tracks-1].push_back(tdat);
        } else if (bssid_gpsnet_map.find(file_points[i]->bssid) == bssid_gpsnet_map.end()) {
            //printf("making new netork: %s\n", file_points[i]->bssid);
            gnet = new gps_network;

            gnet->bssid = file_points[i]->bssid;

            if (bssid_net_map.find(file_points[i]->bssid) != bssid_net_map.end()) {
                gnet->wnet = bssid_net_map[file_points[i]->bssid];

                // Set filter bit as we create it
                /*
                if (type_filter.length() != 0)
                    if (((invert_type_filter == 0 && type_filter.find(NetType2String(gnet->wnet->type)) != string::npos) ||
                         (invert_type_filter == 1 && type_filter.find(NetType2String(gnet->wnet->type)) == string::npos))) {
                        gnet->filtered = 1;;
                    }
                    */
                if ((invert_type_filter == 0 && 
                     type_filter_map.find(gnet->wnet->type) != type_filter_map.end()) ||
                    (invert_type_filter == 1 &&
                     type_filter_map.find(gnet->wnet->type) == type_filter_map.end())) {
                    gnet->filtered = 1;
                }

            } else {
                gnet->wnet = NULL;
            }

            gnet->points.push_back(file_points[i]);

            bssid_gpsnet_map[file_points[i]->bssid] = gnet;

            UpdateGlobalCoords(lat, lon, alt);

        } else {
            gnet = bssid_gpsnet_map[file_points[i]->bssid];

            gnet->points.push_back(file_points[i]);

            UpdateGlobalCoords(lat, lon, alt);

        }
    }

    if (verbose)
        fprintf(stderr, "%s contains %d samples.\n", in_fname, file_samples);

    sample_points += file_samples;

    return 1;
}

// Do all the math
void ProcessNetData(int in_printstats) {
    // Convert the tracks to x,y
    if (draw_track != 0 || draw_power != 0) {
        for (unsigned int vec = 0; vec < track_vec.size(); vec++) {
            for (unsigned int x = 0; x < track_vec[vec].size(); x++) {
                double track_tx, track_ty;
                calcxy(&track_tx, &track_ty, track_vec[vec][x].lat, track_vec[vec][x].lon,
                       (double) map_scale/PIXELFACT, map_avg_lat, map_avg_lon);

                track_vec[vec][x].x = (int) track_tx;
                track_vec[vec][x].y = (int) track_ty;
            }

            if (in_printstats)
                printf("Track %d: %d samples.\n", vec, (int) track_vec[vec].size());
        }
    }

    printf("Processing %d raw networks.\n", (int) bssid_gpsnet_map.size());

    for (map<string, gps_network *>::const_iterator x = bssid_gpsnet_map.begin();
         x != bssid_gpsnet_map.end(); ++x) {

        gps_network *map_iter = x->second;


        if (map_iter->points.size() <= 1) {
           // printf("net %s only had <= 1 point.\n", map_iter->bssid.c_str());
            continue;
        }

        // Calculate the min/max and average sizes of this network
        for (unsigned int y = 0; y < map_iter->points.size(); y++) {
            float lat = map_iter->points[y]->lat;
            float lon = map_iter->points[y]->lon;
            float alt = map_iter->points[y]->alt;
            float spd = map_iter->points[y]->spd;

            //printf("Got %f %f %f %f\n", lat, lon, alt, spd);

            map_iter->avg_lat += lat;
            map_iter->avg_lon += lon;
            map_iter->avg_alt += alt;
            map_iter->avg_spd += spd;
            map_iter->count++;

            // Enter the max/min values
            if (lat > map_iter->max_lat || map_iter->max_lat == 90)
                map_iter->max_lat = lat;

            if (lat < map_iter->min_lat || map_iter->min_lat == -90)
                map_iter->min_lat = lat;

            if (lon > map_iter->max_lon || map_iter->max_lon == 180)
                map_iter->max_lon = lon;

            if (lon < map_iter->min_lon || map_iter->min_lon == -180)
                map_iter->min_lon = lon;

            if (alt > map_iter->max_alt || map_iter->max_alt == 0)
                map_iter->max_alt = alt;

            if (alt < map_iter->min_alt || map_iter->min_alt == 0)
                map_iter->min_alt = alt;
        }

        map_iter->diagonal_distance = earth_distance(map_iter->max_lat, map_iter->max_lon,
                                                    map_iter->min_lat, map_iter->min_lon);

        map_iter->altitude_distance = map_iter->max_alt - map_iter->min_alt;

        double avg_lat = (double) map_iter->avg_lat / map_iter->count;
        double avg_lon = (double) map_iter->avg_lon / map_iter->count;
        double avg_alt = (double) map_iter->avg_alt / map_iter->count;
        double avg_spd = (double) map_iter->avg_spd / map_iter->count;

        map_iter->avg_lat = avg_lat;
        map_iter->avg_lon = avg_lon;
        map_iter->avg_alt = avg_alt;
        map_iter->avg_spd = avg_spd;

        if ((map_iter->diagonal_distance * 3.3) > (20 * 5280))
            printf("WARNING:  Network %s [%s] has range greater than 20 miles, this "
                   "may be a glitch you want to filter.\n", 
                   map_iter->wnet == NULL ? "Unknown" : map_iter->wnet->ssid.c_str(),
                   map_iter->bssid.c_str());
        
        if (in_printstats)
            printf("Net:     %s [%s]\n"
                   "  Samples : %d\n"
                   "  Min lat : %f\n"
                   "  Min lon : %f\n"
                   "  Max lat : %f\n"
                   "  Max lon : %f\n"
                   "  Min alt : %f\n"
                   "  Max Alt : %f\n"
                   "  Avg Lat : %f\n"
                   "  Avg Lon : %f\n"
                   "  Avg Alt : %f\n"
                   "  Avg Spd : %f\n"
                   "  H. Range: %f ft\n"
                   "  V. Range: %f ft\n",
                   map_iter->wnet == NULL ? "Unknown" : map_iter->wnet->ssid.c_str(),
                   map_iter->bssid.c_str(),
                   map_iter->count,
                   map_iter->min_lat, map_iter->min_lon,
                   map_iter->max_lat, map_iter->max_lon,
                   map_iter->min_alt, map_iter->max_alt,
                   map_iter->avg_lat, map_iter->avg_lon,
                   map_iter->avg_alt, map_iter->avg_spd,
                   map_iter->diagonal_distance * 3.3, map_iter->altitude_distance);
    }
}

void AssignNetColors() {
    int base_color = 1;

    for (map<string, gps_network *>::const_iterator x = bssid_gpsnet_map.begin();
         x != bssid_gpsnet_map.end(); ++x) {

        gps_network *map_iter = x->second;

        if (map_iter->filtered)
            continue;

        if (color_coding == COLORCODE_WEP) {
            if (map_iter->wnet != NULL) {
                if (map_iter->wnet->type == network_adhoc || map_iter->wnet->type == network_probe)
                    map_iter->wnet->manuf_ref = MatchBestManuf(client_manuf_map, map_iter->wnet->bssid,
                                                               map_iter->wnet->ssid, map_iter->wnet->channel,
                                                               map_iter->wnet->wep, map_iter->wnet->cloaked,
                                                               &map_iter->wnet->manuf_score);
                else
                    map_iter->wnet->manuf_ref = MatchBestManuf(ap_manuf_map, map_iter->wnet->bssid,
                                                               map_iter->wnet->ssid, map_iter->wnet->channel,
                                                               map_iter->wnet->wep, map_iter->wnet->cloaked,
                                                               &map_iter->wnet->manuf_score);

                if (map_iter->wnet->manuf_score == manuf_max_score) {
                    map_iter->color_index = "#0000FF";
                } else if (map_iter->wnet->wep) {
                    map_iter->color_index = "#FF0000";
                } else {
                    map_iter->color_index = "#00FF00";
                }
            } else {
                map_iter->color_index = "#00FF00";
            }
        } else if (color_coding == COLORCODE_CHANNEL) {
            if (map_iter->wnet != NULL) {
                if (map_iter->wnet->channel < 1 || map_iter->wnet->channel > 14) {
                    map_iter->color_index = channelcolors[0];
                } else {
                    map_iter->color_index = channelcolors[map_iter->wnet->channel - 1];
                }
            } else {
                map_iter->color_index = channelcolors[0];
            }
        } else {
            if (netcolors[base_color] == NULL)
                base_color = 1;

            map_iter->color_index = netcolors[base_color];

            base_color++;
        }
    }
}


// Faust Code to convert rad to deg and find the distance between two points
// on the globe.  Thanks, Faust.
//const float M_PI = 3.14159;

//double rad2deg(double x) { /*FOLD00*/
//     return x*M_PI/180.0;
//}
#define rad2deg(x) ((double)((x)*M_PI/180.0))

double earth_distance(double lat1, double lon1, double lat2, double lon2) { /*FOLD00*/

    /*
    double calcedR1 = calcR(lat1);
    double calcedR2 = calcR(lat2);

    double sinradi1 = sin(rad2deg(90-lat1));
    double sinradi2 = sin(rad2deg(90-lat2));
    
    double x1 = calcedR1 * cos(rad2deg(lon1)) * sinradi1;
    double x2 = calcedR2 * cos(rad2deg(lon2)) * sinradi2;
    double y1 = calcedR1 * sin(rad2deg(lon1)) * sinradi1;
    double y2 = calcedR2 * sin(rad2deg(lon2)) * sinradi2;
    double z1 = calcedR1 * cos(rad2deg(90-lat1));
    double z2 = calcedR2 * cos(rad2deg(90-lat2));
    
    double calcedR = calcR((double)(lat1+lat2)) / 2;
    double a = acos((x1*x2 + y1*y2 + z1*z2)/square(calcedR));
    */

     double x1 = calcR(lat1) * cos(rad2deg(lon1)) * sin(rad2deg(90-lat1));
     double x2 = calcR(lat2) * cos(rad2deg(lon2)) * sin(rad2deg(90-lat2));
     double y1 = calcR(lat1) * sin(rad2deg(lon1)) * sin(rad2deg(90-lat1));     
     double y2 = calcR(lat2) * sin(rad2deg(lon2)) * sin(rad2deg(90-lat2));
     double z1 = calcR(lat1) * cos(rad2deg(90-lat1));
     double z2 = calcR(lat2) * cos(rad2deg(90-lat2));   
     double a = acos((x1*x2 + y1*y2 + z1*z2)/pow(calcR((double) (lat1+lat2)/2),2));
    
    return calcR((double) (lat1+lat2) / 2) * a;
}

// Lifted from gpsdrive 1.7
// CalcR gets the radius of the earth at a particular latitude
// calcxy finds the x and y positions on a 1280x1024 image of a certian scale
//  centered on a given lat/lon.

// This pulls the "real radius" of a lat, instead of a global guesstimate
double calcR (double lat) /*FOLD00*/
{
    double a = 6378.137, r, sc, x, y, z;
    double e2 = 0.081082 * 0.081082;
    /*
     the radius of curvature of an ellipsoidal Earth in the plane of the
     meridian is given by

     R' = a * (1 - e^2) / (1 - e^2 * (sin(lat))^2)^(3/2)

     where a is the equatorial radius,
     b is the polar radius, and
     e is the eccentricity of the ellipsoid = sqrt(1 - b^2/a^2)

     a = 6378 km (3963 mi) Equatorial radius (surface to center distance)
     b = 6356.752 km (3950 mi) Polar radius (surface to center distance)
     e = 0.081082 Eccentricity
     */

    lat = lat * M_PI / 180.0;
    sc = sin (lat);
    x = a * (1.0 - e2);
    z = 1.0 - e2 * sc * sc;
    y = pow (z, 1.5);
    r = x / y;

    r = r * 1000.0;
    return r;
}

void calcxy (double *posx, double *posy, double lat, double lon, double pixelfact, /*FOLD00*/
        double zero_lat, double zero_long) {
    double dif;

    *posx = (calcR(lat) * M_PI / 180.0) * cos (M_PI * lat / 180.0) * (lon - zero_long);

    *posx = (map_width/2) + *posx / pixelfact;
    //*posx = *posx - xoff;

    *posy = (calcR(lat) * M_PI / 180.0) * (lat - zero_lat);

    dif = calcR(lat) * (1 - (cos ((M_PI * (lon - zero_long)) / 180.0)));

    *posy = *posy + dif / 1.85;
    *posy = (map_height/2) - *posy / pixelfact;

    *posx += draw_x_offset;
    *posy += draw_y_offset;

    //*posy = *posy - yoff;
}

// Find the best map scale for the 'rectangle' tlat,tlon
long int BestMapScale(double tlat, double tlon, double blat, double blon) { /*FOLD00*/
    for (int x = 0; scales[x] != 0; x++) {
        double mapx, mapy;
        double map2x, map2y;

        /*
        calcxy (&mapx, &mapy, global_map_avg.max_lat, global_map_avg.max_lon,
                (double) scales[x]/PIXELFACT, map_avg_lat, map_avg_lon);
                */

        calcxy(&mapx, &mapy, tlat, tlon, (double) scales[x]/PIXELFACT, map_avg_lat, map_avg_lon);
        calcxy(&map2x, &map2y, blat, blon, (double) scales[x]/PIXELFACT, map_avg_lat, map_avg_lon);

        if ((mapx < 0 || mapx > map_width || mapy < 0 || mapy > map_height) ||
            (map2x < 0 || map2x > map_width || map2y < 0 || map2y > map_height)) {
            continue;
        } else {
            // Fudge the scale by 10% for extreme ranges
            if (scales[x] >= 1000000 && scales[x] < 20000000)
                return (long) (scales[x] + (scales[x] * 0.10));
            if (scales[x] >= 20000000)
                return (long) (scales[x] + (scales[x] * 0.15));

            return scales[x];
        }
    }

    return 0;
}

//#define geom_distance(a, b, x, y) sqrt(pow((double) (a) - (double) (x), 2) + pow((double) (b) - (double) (y), 2))

// This new distance macro does not use the pow function, for 2 we can simply multiply
// which aves us ~800mio ops !!!!
#define geom_distance(a, b, x, y) sqrt(square((double) (a) - (double) (x)) + square((double) (b) - (double) (y)))

// Frank and Nielson's improved weighting algorithm
double WeightAlgo(int start_x, int start_y, int in_x, int in_y, double in_fuzz, double in_scale) { /*FOLD00*/

    // Step 1:  Find 'r', the distance to the farthest interpolation point
    int min_x = 0, min_y = 0;
    int max_x = map_width, max_y = map_height;
    int offset = (int) (in_fuzz/in_scale);

    if (start_x - offset > min_x)
        min_x = start_x - offset;
    if (start_y - offset > min_y)
        min_y = start_y - offset;
    if (start_x + offset < max_x)
        max_x = start_x + offset;
    if (start_y + offset < max_y)
        max_y = start_y + offset;


//    printf("startx %d starty %d inx %d iny %d\n", start_x, start_y, in_x, in_y);

    // Find the farthest sample point in this set
    double r = 0;
    for (int cury = min_y; cury < max_y; cury++) {
        for (int curx = min_x; curx < max_x; curx++) {
            if (power_input_map[(map_width * cury) + curx] < 0)
                continue;

//            printf("power map at %d,%d has val %d max %d,%d\n", cury, curx,
//                   power_input_map[(map_width * cury) + curx], max_x, max_y);

            double h = geom_distance(start_x, start_y, curx, cury) * 1.2;
            if (h > r)
                r = h;
        }
    }

    // Construct the 'top half' of the weight function:
    double hi = geom_distance(start_x, start_y, in_x, in_y);
    double top_func = square( ((r - hi)/(r * hi)));

    double bot_sum = 0;
    // Construct the 'bottom half' of the weight function
    for (int cury = min_y; cury < max_y; cury++) {
        for (int curx = min_x; curx < max_x; curx++) {
            if (power_input_map[(map_width * cury) + curx] < 0)
                continue;

            double hj = geom_distance(start_x, start_y, curx, cury) * 1.8;

            bot_sum += square( ((r - hj)/(r * hj)));
        }
    }

    // Put it all together and return the influence
    double weight = top_func/bot_sum;

    return weight;
}

// Inverse weight calculations -- Shepard's with Frank and Nielson's improved weight
// algorithm
int InverseWeight(int in_x, int in_y, int in_fuzz, double in_scale) { /*FOLD00*/
    int min_x = 0, min_y = 0;
    int max_x = map_width, max_y = map_height;

    int offset = (int)(100 * (double) (1 / in_scale));

    // Moved the abort to here, so we don't need to do the downward things
    if (offset == 0)
        return 0;

    if (in_x - offset > min_x)
        min_x = in_x - offset;
    if (in_y - offset > min_y)
        min_y = in_y - offset;
    if (in_x + offset < max_x)
        max_x = in_x + offset;
    if (in_y + offset < max_y)
        max_y = in_y + offset;

    /*
    fprintf(stderr, "influenced by %d range, %d %d from %d %d to %d %d\n",
            offset, in_x, in_y, min_x, min_y, max_x, max_y);
            */


    double power_sum = 0;

    for (int cury = min_y; cury < max_y; cury++) {
        for (int curx = min_x; curx < max_x; curx++) {
            // Round out the distance we calc in...  Store some stuff to make
            // math faster
            double ldist = sqrt(((in_x - curx)*(in_x - curx)) +
                    ((in_y - cury)*(in_y - cury)));

            if ((int) ldist > offset) {
                continue;
            }
            
            if (power_input_map[(map_width * cury) + curx] < 0)
                continue;

            power_sum += WeightAlgo(in_x, in_y, curx, cury, in_fuzz, in_scale) * power_input_map[(map_width * cury) + curx];

        }
    }

    return (int) power_sum;

}

void DrawNetTracks(Image *in_img, DrawInfo *in_di) { /*FOLD00*/
    // Our track color
    uint8_t track_r = 0x00, track_g = 0x00, track_b = 0xFF;
    char color_str[8];
    PixelPacket track_clr;

    // Draw each track
    for (unsigned int vec = 0; vec < track_vec.size(); vec++) {
        if (track_vec[vec].size() == 0)
            continue;

        // Generate the color we're drawing with
        snprintf(color_str, 8, "#%02X%02X%02X", track_r, track_g, track_b);

        ExceptionInfo excep;
        GetExceptionInfo(&excep);
	QueryColorDatabase(color_str, &track_clr, &excep);
        if (excep.severity != UndefinedException) {
            CatchException(&excep);
            break;
        }

        in_di->stroke = track_clr;

        // Dim the color
        track_b -= track_decay;

        // Reset it if we're "too dark"
        if (track_b < 0x50)
            track_b = 0xFF;

        // Initialize the previous track location vars

        int prev_tx, prev_ty;
        prev_tx = track_vec[vec][0].x;
        prev_ty = track_vec[vec][0].y;

        for (unsigned int x = 1; x < track_vec[vec].size(); x++) {
            char prim[1024];

            // If we don't have a previous vector (ie, the map data failed), set it
            // and continue
            if (prev_tx == -1 || prev_ty == -1) {
                prev_tx = track_vec[vec][x].x;
                prev_ty = track_vec[vec][x].y;
                continue;
            }

            // Scrap dupes
            if (track_vec[vec][x].x == (unsigned int) prev_tx &&
                track_vec[vec][x].y == (unsigned int) prev_ty)
                continue;

            // Scrap stuff entirely off-screen to save on speed
            if (((unsigned int) prev_tx > map_width && (unsigned int) prev_ty > map_height &&
                 track_vec[vec][x].x > map_width && track_vec[vec][x].y > map_height) ||
                (prev_tx < 0 && prev_ty < 0 &&
                 track_vec[vec][x].x < 0 && track_vec[vec][x].y < 0)) {

                continue;
            }

            // If the track jumps more than 50 meters in 1 second, assume we had a
            // problem and restart the track at the next position
            double distance;
            if ((distance = geom_distance(track_vec[vec][x].x, track_vec[vec][x].y,
                                          prev_tx, prev_ty)) > 50) {
                prev_tx = -1;
                prev_ty = -1;
                continue;
            }

            /* Don't whine about track jumps (for now)
            if (sqrt(pow(track_vec[vec][x].x - prev_tx, 2) + pow(track_vec[vec][x].y - prev_ty, 2)) > 20) {
                printf("Suspicious track record: %dx%d (%fx%f)\n"
                       "Prev: %dx%d (%fx%f)\n",
                       track_vec[vec][x].x, track_vec[vec][x].y,
                       track_vec[vec][x].lat, track_vec[vec][x].lon,
                       prev_tx, prev_ty,
                       track_vec[vec][x-1].lat, track_vec[vec][x-1].lon);
                       }
                       */

            // fill-opacity %d%% stroke-opacity %d%%
            snprintf(prim, 1024, "stroke-width %d line %d,%d %d,%d",
                     track_width,
                     prev_tx, prev_ty, track_vec[vec][x].x, track_vec[vec][x].y);

            //in_di->primitive = strdup(prim);
            in_di->primitive = prim;
            DrawImage(in_img, in_di);
            GetImageException(in_img, &im_exception);
            if (im_exception.severity != UndefinedException) {
                CatchException(&im_exception);
                break;
            }

            prev_tx = track_vec[vec][x].x;
            prev_ty = track_vec[vec][x].y;
        }
    }
}

void DrawNetCircles(vector<gps_network *> in_nets, Image *in_img, DrawInfo *in_di) { /*FOLD00*/
    for (unsigned int x = 0; x < in_nets.size(); x++) {
        gps_network *map_iter = in_nets[x];

        // Skip networks w/ no determined coordinates
        if (map_iter->max_lat == 90)
            continue;

        if (map_iter->diagonal_distance > horiz_throttle)
            continue;

        /*
        // Figure x, y of min on our hypothetical map
        double mapx, mapy;

        calcxy (&mapx, &mapy, map_iter->avg_lat, map_iter->avg_lon,
                (double) map_scale/PIXELFACT, map_avg_lat, map_avg_lon);

        double end_lat, end_lon;

        // Find the nearest corner of the bounding rectangle, this will determine
        // the size of our network circle
        if (((map_iter->min_lat + map_iter->max_lat) / 2) < map_iter->avg_lat)
            end_lat = map_iter->max_lat;
        else
            end_lat = map_iter->min_lat;
        if (((map_iter->min_lon + map_iter->max_lon)/ 2) < map_iter->avg_lon)
            end_lon = map_iter->max_lon;
        else
            end_lon = map_iter->min_lon;

        double endx, endy;
        calcxy(&endx, &endy, end_lat, end_lon,
               (double) map_scale/PIXELFACT, map_avg_lat, map_avg_lon);

        // printf("  Endpt   : %dx%d\n", (int) endx, (int) endy);
        */

        // Find the average distance to all the points in the network and use it as
        // the radius
        if (map_iter->points.size() == 0)
            continue;

        double mapx, mapy, endx, endy;
        calcxy (&mapx, &mapy, map_iter->avg_lat, map_iter->avg_lon,
                (double) map_scale/PIXELFACT, map_avg_lat, map_avg_lon);

        double distavg = 0;
        for (unsigned int y = 0; y < map_iter->points.size(); y++) {
            gps_point *pt = map_iter->points[y];

            double ptx, pty;
            calcxy(&ptx, &pty, pt->lat, pt->lon, (double) map_scale/PIXELFACT,
                   map_avg_lat, map_avg_lon);

            distavg += labs((long) geom_distance(mapx, mapy, ptx, pty));
        }
        distavg = distavg / map_iter->points.size();

        if (!finite(distavg))
            continue;

        endx = mapx + distavg;
        endy = mapy + distavg;

        if (!(((mapx - distavg > 0 && mapx - distavg < map_width) &&
               (mapy - distavg > 0 && mapy - distavg < map_width)) &&
              ((endx > 0 && endx < map_height) &&
               (endy > 0 && endy < map_height)))) {
            continue;
        } 

        drawn_net_map[map_iter->bssid.c_str()] = map_iter;
        
        PixelPacket netclr;

        ExceptionInfo excep;
        GetExceptionInfo(&excep);
        QueryColorDatabase(map_iter->color_index.c_str(), &netclr, &excep);
        if (excep.severity != UndefinedException) {
            CatchException(&excep);
            break;
        }

        in_di->fill = netclr;
        in_di->stroke = netclr;

        char prim[1024];

        snprintf(prim, 1024, "fill-opacity %d%% stroke-opacity %d%% circle %d,%d %d,%d",
                 range_opacity, range_opacity, (int) mapx, (int) mapy, (int) endx, (int) endy);

        in_di->primitive = prim;
        DrawImage(in_img, in_di);
        GetImageException(in_img, &im_exception);
        if (im_exception.severity != UndefinedException) {
            CatchException(&im_exception);
            break;
        }
    }
}

double clockwize( int x0, int y0, int x1, int y1, int x2, int y2) { /*FOLD00*/
	return ( x2 - x0 ) * ( y1 - y0 ) - ( x1 - x0 ) * ( y2 - y0 );
}

void DrawNetHull(vector<gps_network *> in_nets, Image *in_img, DrawInfo *in_di) { /*FOLD00*/
    for (unsigned int x = 0; x < in_nets.size(); x++) {
        gps_network *map_iter = in_nets[x];

        // Skip networks w/ no determined coordinates
        if (map_iter->max_lat == 90)
            continue;

        if (map_iter->diagonal_distance > horiz_throttle)
            continue;

        map<string, hullPoint> dim;
        for (unsigned int x = 0; x < map_iter->points.size(); x++) {
            gps_point *pt = map_iter->points[x];
            double mapx, mapy;

            calcxy (&mapx, &mapy, pt->lat, pt->lon,
                    (double) map_scale/PIXELFACT, map_avg_lat, map_avg_lon);

            // This is faily inefficient but what the hell.
            if ((mapx > 0 && mapx < map_width) || (mapy > 0 && mapy < map_height))
                drawn_net_map[map_iter->bssid.c_str()] = map_iter;

            char mm1[64];
            snprintf(mm1, 64, "%d,%d", (int) mapx, (int) mapy);
            string a = mm1;
            hullPoint b;
            b.x = (int) mapx;
            b.y = (int) mapy;
            b.angle = 0.0;
            b.xy = a;
            dim[a] = b;
	}

	// need at least 3 points for a hull

	//printf("\nPts: %d\n", dim.size());
	if (dim.size() < 3)
		continue;

	// got the unique points, now we need to sort em
        deque<hullPoint> pts;
	for (map<string, hullPoint>::const_iterator i = dim.begin(); i != dim.end(); ++i) {
		pts.push_back(i->second);
	}
	stable_sort(pts.begin(), pts.end());

	//start point for the hull
	hullPoint start = pts[0];
	pts.pop_front();

	//compute angles for pts
	for (deque<hullPoint>::iterator j = pts.begin(); j != pts.end(); ++j) {
		j->angle = atan2( j->y - start.y, j->x - start.x );
	}

	//sort against angle
	stable_sort(pts.begin(), pts.end(), hullPoint() );

	//build the hull
	vector<hullPoint> hull;
	hull.push_back(start);
	hullPoint tmp = pts[0];
	hull.push_back(tmp);
	pts.push_front(start);

        for (unsigned int k = 2; k < pts.size() ; k++) {
            while (clockwize(hull[hull.size()-2].x,
                             hull[hull.size()-2].y,
                             hull[hull.size()-1].x,
                             hull[hull.size()-1].y,
                             pts[k].x,
                             pts[k].y) >= 0
                   && hull.size() >= 2) {
                hull.pop_back();
            }
            hull.push_back(pts[k]);
        }

        if (hull.size() < 3)
            continue;
			
	//wheh
        /*
         printf("Hull:\n");
         for(vector<hullPoint>::const_iterator l = hull.begin(); l != hull.end(); ++l) {
         printf("x: %d y: %d a: %f\n", l->x, l->y, l->angle);
         }
         printf("orig:\n");
         for(deque<hullPoint>::const_iterator l = pts.begin(); l != pts.end(); ++l) {
         printf("x: %d y: %d a: %f\n", l->x, l->y, l->angle);
         }
         */

        PixelPacket netclr;

        ExceptionInfo excep;
        GetExceptionInfo(&excep);
        QueryColorDatabase(map_iter->color_index.c_str(), &netclr, &excep);
        if (excep.severity != UndefinedException) {
            CatchException(&excep);
            break;
        }

        in_di->fill = netclr;

        string sep = ", ";
        string pstr = "";
        for(vector<hullPoint>::const_iterator l = hull.begin(); l != hull.end(); ++l) {
            pstr = pstr + l->xy + sep;
	}
        pstr = pstr + start.xy;

        char pstr2[2048];
        memset(pstr2, 0, sizeof(char)*2048);
	pstr.copy(pstr2, string::npos);

	char prim[2048];
        snprintf(prim, 1024, "fill-opacity %d%% stroke-opacity %d%% polygon %s",
                 hull_opacity, hull_opacity, pstr2);

	//printf("%s\n", prim);
	
        in_di->primitive = prim;
        DrawImage(in_img, in_di);
        GetImageException(in_img, &im_exception);
        if (im_exception.severity != UndefinedException) {
            CatchException(&im_exception);
            break;
        }

    }

}

void DrawNetBoundRects(vector<gps_network *> in_nets, Image *in_img, DrawInfo *in_di, /*FOLD00*/
                       int in_fill) {
    for (unsigned int x = 0; x < in_nets.size(); x++) {
        gps_network *map_iter = in_nets[x];

        // Skip networks w/ no determined coordinates
        if (map_iter->max_lat == 90)
            continue;

        if (isnan(map_iter->diagonal_distance) || map_iter->diagonal_distance == 0 || 
            map_iter->diagonal_distance > horiz_throttle)
            continue;

        // Figure x, y of min on our hypothetical map
        double mapx, mapy;
        calcxy (&mapx, &mapy, map_iter->max_lat, map_iter->max_lon,
                (double) map_scale/PIXELFACT, map_avg_lat, map_avg_lon);

        double endx, endy;
        calcxy(&endx, &endy, map_iter->min_lat, map_iter->min_lon,
               (double) map_scale/PIXELFACT, map_avg_lat, map_avg_lon);

        double tlx, tly, brx, bry;

        if (mapx < endx) {
            tlx = mapx;
            brx = endx;
        } else {
            tlx = endx;
            brx = mapx;
        }

        if (mapy < endy) {
            tly = mapy;
            bry = endy;
        } else {
            tly = endy;
            bry = mapy;
        }

        if (!(((tlx > 0 && tlx < map_width) &&
               (tly > 0 && tly < map_width)) &&
              ((brx > 0 && brx < map_height) &&
               (bry > 0 && bry < map_height)))) {
            continue;
        } 
        drawn_net_map[map_iter->bssid.c_str()] = map_iter;

        if (in_fill) {
            PixelPacket netclr;

            ExceptionInfo excep;
            GetExceptionInfo(&excep);
            QueryColorDatabase(map_iter->color_index.c_str(), &netclr, &excep);
            if (excep.severity != UndefinedException) {
                CatchException(&excep);
                break;
            }

            in_di->fill = netclr;
        }

        char prim[1024];

        snprintf(prim, 1024, "stroke black fill-opacity %d%% rectangle %d,%d %d,%d",
                 in_fill, (int) mapx, (int) mapy, (int) endx, (int) endy);

        //snprintf(prim, 1024, "fill-opacity %d%% rectangle %d,%d %d,%d",
        //in_fill, (int) tlx, (int) tly, (int) brx, (int) bry);


        in_di->primitive = prim;
        DrawImage(in_img, in_di);
        GetImageException(in_img, &im_exception);
        if (im_exception.severity != UndefinedException) {
            CatchException(&im_exception);
            break;
        }

        /*
        //d = sqrt[(x1-x2)^2 + (y1-y2)^2]
        printf("  Px RLen : %d\n", (int) sqrt(pow((int) mapx - endx, 2) + pow((int) mapy - endy, 2)));
        */

    }
}

// Thread function to compute a line of interpolated data

typedef struct powerline_arg {
//    unsigned int y;
//    unsigned int y_max;
    unsigned int in_res;
    unsigned int threadno;
};

void *PowerLine(void *arg) {
    powerline_arg *parg = (powerline_arg *) arg;
    time_t startline;

//    unsigned int y_offset = parg->y;
//    unsigned int y_max = parg->y_max;
    unsigned int in_res = parg->in_res;
    unsigned int y = 0;

    while (y < map_height) {
#ifdef HAVE_PTHREAD
        pthread_mutex_lock(&power_pos_lock);
#endif
        y = power_pos * in_res;
        power_pos++;
#ifdef HAVE_PTHREAD
        pthread_mutex_unlock(&power_pos_lock);
#endif
        if (y >= map_height)
            break;

        //    for (unsigned int y = y_offset; y < map_height; y+= (in_res * numthreads)) 
        startline = time(0);

#ifdef HAVE_PTHREAD
        pthread_mutex_lock(&print_lock);
        fprintf(stderr, "Thread %d: crunching interpolation image line %d\n", parg->threadno, y);
        pthread_mutex_unlock(&print_lock);
#else
        fprintf(stderr, "Crunching interpolation image line %d\n", y);
#endif

        for (unsigned int x = 0; x < map_width; x+= in_res) {
            unsigned int powr = InverseWeight(x, y, (int) (map_scale/200),
                                              (double) map_scale/PIXELFACT);

#ifdef HAVE_PTHREAD
            pthread_mutex_lock(&power_lock);
#endif
            power_map[(map_width * y) + x] = powr;
#ifdef HAVE_PTHREAD
            pthread_mutex_unlock(&power_lock);
#endif
        }

        if (verbose) {
#ifdef HAVE_PTHREAD
            pthread_mutex_lock(&print_lock);
#endif
            int elapsed = time(0) - startline;
            int complet = elapsed * ((map_height - y) / in_res);

            fprintf(stderr, "Completed in %d seconds.  (Estimated: %dh %dm %ds to completion)\n",
                    elapsed, (complet/60)/60, (complet/60) % 60, complet % 60);
#ifdef HAVE_PTHREAD
            pthread_mutex_unlock(&print_lock);
#endif
        }

    }

#ifdef HAVE_PTHREAD
    pthread_exit((void *) 0);
    return NULL;
#else
    return NULL;
#endif
}

void DrawNetPower(vector<gps_network *> in_nets, Image *in_img, 
                  DrawInfo *in_di) {
//    PixelPacket point_clr;
#ifdef HAVE_PTHREAD
    pthread_attr_t attr;
#endif

    power_map = new unsigned int [map_width * map_height];
//    memset(power_map, 0, sizeof(unsigned int) * (map_width * map_height));
    //    This is not really needed, the image is ok althoug ( please verify )
    //    so we can save about 1.3mio opcodes here

    power_input_map = new int [map_width * map_height];
    memset(power_input_map, -1, sizeof(int) * (map_width * map_height));

    for (unsigned int x = 0; x < in_nets.size(); x++) {
        gps_network *map_iter = in_nets[x];

        for (unsigned int y = 0; y < map_iter->points.size(); y++) {
            // unsigned int curx = track_vec[vec][i].x, cury = track_vec[vec][i].y;
            double dcurx, dcury;

            calcxy(&dcurx, &dcury, map_iter->points[y]->lat, map_iter->points[y]->lon,
                   (double) map_scale/PIXELFACT, map_avg_lat, map_avg_lon);

            unsigned int curx = (unsigned int) dcurx, cury = (unsigned int) dcury;

            if (curx >= map_width || cury >= map_height || curx < 0 || cury < 0)
                continue;

            /*
            printf("comparing map %d,%d val %d to %d\n", curx, cury,
                   power_input_map[(map_width * cury) + curx], 
                   map_iter->points[y]->signal);
                   */

            if (power_input_map[(map_width * cury) + curx] < 
                map_iter->points[y]->signal && map_iter->points[y]->signal != 0) {
                power_input_map[(map_width * cury) + curx] = map_iter->points[y]->signal;
                if (map_iter->points[y]->signal < signal_lowest)
                    signal_lowest = map_iter->points[y]->signal;
                if (map_iter->points[y]->signal > signal_highest)
                    signal_highest = map_iter->points[y]->signal;
            }
        }

    }

    // Attempt to normalize the data somewhat
    int median = (signal_lowest + signal_highest) / 2;
    for (unsigned int v = 0; v < map_height; v++) {
        for (unsigned int h = 0; h < map_width; h++) {
            if (power_input_map[(map_width * v) + h] != 0)
                power_input_map[(map_width * v) + h] -= median;
        }
    }

#if 0
    // Convert the power data in the tracks into a 2d map
    fprintf(stderr, "Converting track power data to coordinate mesh...\n");
    for (unsigned int vec = 0; vec < track_vec.size(); vec++) {
        for(unsigned int i = 0; i < track_vec[vec].size(); i++) {

            if (track_vec[vec][i].version < 2)
                continue;

            unsigned int curx = track_vec[vec][i].x, cury = track_vec[vec][i].y;

            if (curx >= map_width || cury >= map_height || curx < 0 || cury < 0)
                continue;

            /*
            printf("comparing map %d,%d val %d to %d\n", curx, cury,
            power_input_map[(map_width * cury) + curx], track_vec[vec][i].power);
            */

            if (power_input_map[(map_width * cury) + curx] < track_vec[vec][i].power &&
                track_vec[vec][i].power != 0) {
                power_input_map[(map_width * cury) + curx] = track_vec[vec][i].power;
            }

        }
    }
#endif

    fprintf(stderr, "Interpolating power into graph points.\n");

    powerline_arg *pargs;
#ifdef HAVE_PTHREAD
    // Slice the map into pieces and assign it to the threads, averaging high - if it's
    // not evenly divisible the last thread may get less work to do than the others.
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    pargs = new powerline_arg[numthreads];
    for (unsigned int t = 0; t < numthreads; t++) {
        pargs[t].in_res = power_resolution;
        pargs[t].threadno = t;

        pthread_create(&mapthread[t], &attr, PowerLine, (void *) &pargs[t]);
    }

    pthread_attr_destroy(&attr);

    // Now wait for the threads to complete and come back
    int thread_status;
    for (unsigned int t = 0; t < numthreads; t++) {
        pthread_join(mapthread[t], (void **) &thread_status);
    }
#else
    // Run one instance of our "thread".  thread number 0, it should just crunch it all
    pargs = new powerline_arg;
    pargs->in_res = power_resolution;
    pargs->threadno = 0;

    PowerLine((void *) pargs);
#endif
    fprintf(stderr, "Preparing colormap.\n");
    // Doing this here saves us another ~ 1mio operations
    PixelPacket colormap[power_steps];
    ExceptionInfo ex;
    GetExceptionInfo(&ex);
    for ( int i = 0; i < power_steps; i++) {
        QueryColorDatabase(power_colors[i], &colormap[i], &ex);
        if ( ex.severity != UndefinedException ) {
            CatchException(&ex);
            break;
        }
    }

    ExceptionInfo excep;
    GetExceptionInfo(&excep);

    // Since the opacity value is always the same, we can 
    // generate the template before the loop, which saves
    // us another 1.5mio ops

    char * point_template = new char[1024];
    char * rect_template = new char[1024];

    snprintf(point_template , 1024, "fill-opacity %d%%%% stroke-opacity %d%%%% stroke-width 0 point %%d,%%d", power_opacity, power_opacity);
    snprintf(rect_template  , 1024, "fill-opacity %d%%%% stroke-opacity %d%%%% stroke-width 0 rectangle %%d,%%d %%d,%%d", power_opacity, power_opacity);
    
    fprintf(stderr, "Drawing interpolated power levels to map.\n");

    int power_stepsize = (signal_highest - signal_lowest) / power_steps;
    int power_halfstepsize = power_stepsize;
    
    for (unsigned int y = 0; y < map_height; y += power_resolution) {
        for (unsigned int x = 0; x < map_width; x += power_resolution) {
            //            int powr = InverseWeight(x, y, 200, (double) scale/PIXELFACT);
            int powr = power_map[(map_width * y) + x];

            //printf("Got weight %d for pixel %d,%d\n", powr, x, y);

            if (powr > 0 && powr > power_halfstepsize ) {

                int power_index = powr / power_stepsize;

                if (power_index >= power_steps)
                    power_index = power_steps - 1;

		/*
                QueryColorDatabase(power_colors[power_index], &point_clr, &excep);
                if (excep.severity != UndefinedException) {
                    CatchException(&excep);
                    break;
                }*/

		/*
                in_di->stroke = point_clr;
                in_di->fill = point_clr;
		*/

                in_di->stroke = colormap[power_index];
                in_di->fill = colormap[power_index];

                char prim[1024];

                int b, r;

                if (power_resolution == 1) {
                    snprintf(prim, 1024, point_template, x, y);
                } else {
                    r = x + power_resolution - 1;
                    b = y + power_resolution - 1;
                    snprintf(prim, 1024, rect_template, x, y, r, b);
                }

                //printf("%d,%d power %d\n", x, y, powr);
                //snprintf(prim, 1024, "fill-opacity %d%% stroke-opacity %d%% stroke-width 1 rectangle %d,%d %d,%d",
                //         draw_opacity, draw_opacity, x, y, r, b);

                in_di->primitive = prim;
                DrawImage(in_img, in_di);
                GetImageException(in_img, &im_exception);
                if (im_exception.severity != UndefinedException) {
                    CatchException(&im_exception);
                    break;
                }
            }
        }

    }

    delete[] power_map;
    delete[] power_input_map;
    delete point_template;
    delete rect_template;
}

void DrawNetCenterDot(vector<gps_network *> in_nets, Image *in_img, DrawInfo *in_di) {
    for (unsigned int x = 0; x < in_nets.size(); x++) {
        gps_network *map_iter = in_nets[x];

        // Skip networks w/ no determined coordinates
        if (map_iter->max_lat == 90)
            continue;

        if (map_iter->diagonal_distance > horiz_throttle)
            continue;


        // Figure x, y of min on our hypothetical map
        double mapx, mapy;

        calcxy (&mapx, &mapy, map_iter->avg_lat, map_iter->avg_lon,
                (double) map_scale/PIXELFACT, map_avg_lat, map_avg_lon);

        if (!((mapx > 0 && mapx < map_width) || (mapy > 0 && mapy < map_height)))
            continue;

        drawn_net_map[map_iter->bssid.c_str()] = map_iter;

        double endx, endy;
        endx = mapx + center_resolution;
        endy = mapy + center_resolution;

        // printf("  Endpt   : %dx%d\n", (int) endx, (int) endy);

        PixelPacket netclr;

        ExceptionInfo excep;
        GetExceptionInfo(&excep);
        QueryColorDatabase(map_iter->color_index.c_str(), &netclr, &excep);
        if (excep.severity != UndefinedException) {
            CatchException(&excep);
            break;
        }

        in_di->fill = netclr;
        in_di->stroke = netclr;

        char prim[1024];

        snprintf(prim, 1024, "fill-opacity 100%% stroke-opacity 100%% circle %d,%d %d,%d",
                 (int) mapx, (int) mapy, (int) endx, (int) endy);
        in_di->primitive = prim;
        DrawImage(in_img, in_di);
        GetImageException(in_img, &im_exception);
        if (im_exception.severity != UndefinedException) {
            CatchException(&im_exception);
            break;
        }

    }
}

void DrawNetCenterText(vector<gps_network *> in_nets, Image *in_img, DrawInfo *in_di) {
    for (unsigned int x = 0; x < in_nets.size(); x++) {
        gps_network *map_iter = in_nets[x];

        // Skip networks w/ no determined coordinates
        if (map_iter->max_lat == 90)
            continue;

        if (map_iter->diagonal_distance > horiz_throttle)
            continue;

        // Figure x, y of min on our hypothetical map
        double mapx, mapy;

        calcxy (&mapx, &mapy, map_iter->avg_lat, map_iter->avg_lon,
                (double) map_scale/PIXELFACT, map_avg_lat, map_avg_lon);

        PixelPacket netclr;

        char prim[1024];

        ExceptionInfo excep;
        GetExceptionInfo(&excep);
        QueryColorDatabase("#000000", &netclr, &excep);
        if (excep.severity != UndefinedException) {
            CatchException(&excep);
            break;
        }

        in_di->fill = netclr;
        in_di->stroke = netclr;

        char text[1024];
        text[0] = '\0';

        // Do we not have a draw condition at all, so we stop doing this
        int draw = 0;
        // Do we just not have a draw condition for this network
        int thisdraw = 0;

        if (network_labels.find("name") != string::npos) {
            if (map_iter->wnet != NULL) {
		if (map_iter->wnet->beacon_info == "") {
		    snprintf(text, 1024, "%s", map_iter->wnet->ssid.c_str());
		} else {
		    snprintf(text, 1024, "%s/%s", map_iter->wnet->ssid.c_str(),
					map_iter->wnet->beacon_info.c_str());
		}
                thisdraw = 1;
            }
            draw = 1;
        }

        if (network_labels.find("bssid") != string::npos) {
	    char text2[1024];
            snprintf(text2, 1024, "%s (%s)", text, map_iter->bssid.c_str());
            strncpy(text, text2, 1024);
            draw = 1;
            thisdraw = 1;
        }

        if (network_labels.find("manuf") != string::npos) {
            if (map_iter->wnet != NULL) {
		map_iter->wnet->manuf_ref = MatchBestManuf(ap_manuf_map, map_iter->wnet->bssid,
			       map_iter->wnet->ssid, map_iter->wnet->channel,
			       map_iter->wnet->wep, map_iter->wnet->cloaked,
			       &map_iter->wnet->manuf_score);
		if (map_iter->wnet->manuf_ref) {
		    char text2[1024];
		    snprintf(text2, 1024, "%s %s", text, map_iter->wnet->manuf_ref->name.c_str());
		    strncpy(text, text2, 1024);
		}
		draw = 1;
		thisdraw = 1;
	    }
        }

        if (thisdraw == 0)
            continue;

        if (draw == 0)
            break;

        // Catch this just in case since
        if (strlen(text) == 0)
            continue;

        in_di->text = text;
        TypeMetric metrics;
        if (!GetTypeMetrics(in_img, in_di, &metrics)) {
            GetImageException(in_img, &im_exception);
            if (im_exception.severity != UndefinedException) {
                CatchException(&im_exception);
                break;
            }
            continue;
        }

        // Find the offset we're using
        int xoff, yoff;
        switch (label_orientation) {
        case 0:
            xoff = -4 + (int) metrics.height / 3;
            yoff = -4 - (int) metrics.width;
            break;
        case 1:
            xoff = -4 + (int) metrics.height / 3;
            yoff = 0 - (int) metrics.width / 2;
            break;
        case 2:
            xoff = -4 + (int) metrics.height / 3;
            yoff = 4;
            break;
        case 3:
            xoff = 0 + (int) metrics.height / 3;
            yoff = -4 - (int) metrics.width;
            break;
        case 4:
            xoff = 0 + (int) metrics.height / 3;
            yoff = 0 - (int) metrics.width / 2;
            break;
        case 5:
            xoff = 0 + (int) metrics.height / 3;
            yoff = 4;
            break;
        case 6:
            xoff = 4 - (int) metrics.height / 3;
            yoff = -4 - (int) metrics.width / 2;
            break;
        case 7:
            xoff = 4 - (int) metrics.height / 3;
            yoff = 0 - (int) metrics.width / 2;
            break;
        case 8:
            xoff = 4 - (int) metrics.height / 3;
            yoff = 4;
            break;
        default:
            xoff = 0 + (int) metrics.height / 3;
            yoff = 0 - (int) metrics.width / 2;
            break;
        }

	map_iter->label.x = (int) mapx + yoff;
	map_iter->label.y = (int) mapy + xoff;
	map_iter->label.h = (int) metrics.height;
	map_iter->label.w = (int) metrics.width;

	while (1) {
	    unsigned int y;
	    for (y = 0; y < x; y++) {
		gps_network *map_iter1 = in_nets[y];
		if ((map_iter1->label.x + map_iter1->label.w > map_iter->label.x)
		 && (map_iter1->label.x < map_iter->label.x + map_iter->label.w)
		 && (map_iter1->label.y + map_iter1->label.h > map_iter->label.y)
		 && (map_iter1->label.y < map_iter->label.y + map_iter->label.h)) {
			map_iter->label.y = map_iter1->label.y + map_iter1->label.h;
			break;
		}
	    }
	    if (x == y) break;
	}

        snprintf(prim, 1024, "fill-opacity 100%% stroke-opacity 100%% text %d,%d \"%s\"",
                 map_iter->label.x, map_iter->label.y, text);

        in_di->primitive = prim;
        DrawImage(in_img, in_di);
        GetImageException(in_img, &im_exception);
        if (im_exception.severity != UndefinedException) {
            CatchException(&im_exception);
            break;
        }
    }
}


void DrawNetScatterPlot(vector<gps_network *> in_nets, Image *in_img, DrawInfo *in_di) { /*FOLD00*/
    int power_level=0;
    int power_level_total=0;
    int coord_count=0;
    int power_index=0;
    float threshold=0;
    int max_power_level=0;

    for (unsigned int x = 0; x < in_nets.size(); x++) {
        gps_network *map_iter = in_nets[x];

        // Skip networks w/ no determined coordinates
        /* ATR - Removed this check because it skips sites with readings at only a single coordinate. Hope it doesn't break something else.........
        if (map_iter->max_lat == 0)
            continue;
        */

        if (map_iter->diagonal_distance > horiz_throttle)
            continue;

	// hehe, cheating with a hash
        map<string, string> dim;
        map<string, int> dim_signal_total; // For power plotting
        map<string, int> dim_count; // For power plotting
        for (unsigned int y = 0; y < map_iter->points.size(); y++) {
            gps_point *pt = map_iter->points[y];

            double mapx, mapy;
            calcxy (&mapx, &mapy, pt->lat, pt->lon, (double) map_scale/PIXELFACT,
                    map_avg_lat, map_avg_lon);

            double endx, endy;
            endx = mapx + scatter_resolution;
            endy = mapy + scatter_resolution;

            char mm1[64];
            snprintf(mm1, 64, "%d,%d", (int) mapx, (int) mapy);
            char mm2[64];
            snprintf(mm2, 64, "%d,%d", (int) endx, (int) endy);
            string a = mm1;
            string b = mm2;
            dim[a] = b;
            dim_signal_total[a] = dim_signal_total[a] + (int) pt->signal; //ATR associative array for commulative signal values seen at this coordinate
            dim_count[a]++; //ATR associative array for number of entries seen at this coordinate

            // ATR Find highest power reading for dynamic scaling
            if ((int) pt->signal > max_power_level) {
                max_power_level = (int) pt->signal;
            }
        }

        if ( scatter_power == 0) {
            // If regular network based coloring, go ahead and set network color
            PixelPacket netclr;

            ExceptionInfo excep;
            GetExceptionInfo(&excep);
            QueryColorDatabase(map_iter->color_index.c_str(), &netclr, &excep);
            if (excep.severity != UndefinedException) {
                CatchException(&excep);
                break;
            }

            in_di->fill = netclr;
        }  else {

                // ATR Determine range value for assigning colors
                if (power_zoom > 0) {
                        max_power_level = power_zoom;
                }
                if(max_power_level < power_steps) {  // ATR don't break down further than number of colors
                        threshold = 1;
                } else {
                        threshold = (float) max_power_level/(float) (power_steps);

                }
                printf("Power Zoom=%d : Power Steps=%d : Range for Color Index=%.2f\n", max_power_level, power_steps, threshold);
        }

        for (map<string, string>::const_iterator y = dim.begin(); y != dim.end(); ++y) {
            if (scatter_power == 1) { // ATR If power based coloring, determine and set color for each scatter point

                        // ATR calc average power from multiple values
                        power_level_total = dim_signal_total[y->first];
                        coord_count = dim_count[y->first];
                        if (power_level_total == 0) { //ATR sig is really something above zero or we wouldn't get a packet ;)
                                power_level_total++;
                        }
                        power_level = power_level_total/coord_count;
                        if (power_level == 0) { //ATR sig is really something above zero or we wouldn't get a packet ;)
                                power_level++;
                        }

                        // ATR Determine color index
                        power_index = (int) (power_level / threshold);
                        if (power_index == 0) { // ATR value of zero means we got a bogus integer rounding number
                                power_index++;
                        }
                        if (power_index > power_steps) { //ATR if user specifies zoom that's less than max_power, then set index to highest color
                                power_index = power_steps;
                        }

                        PixelPacket netclr;
                        ExceptionInfo excep;
                        GetExceptionInfo(&excep);
                        QueryColorDatabase(power_colors[power_index-1], &netclr, &excep); // ATR - Get color based on signal power
                        if (excep.severity != UndefinedException) {
                                CatchException(&excep);
                                break;
                        }
                        in_di->fill = netclr;
                        printf("Plot=%s : Commulative Power=%d : Number Readings=%d : Ave Power=%d : Color Index=%d \n", y->first.c_str(), power_level_total, coord_count, power_level, power_index);
            }




            char prim[1024];

            snprintf(prim, 1024, "fill-opacity %d%% stroke-opacity %d%% circle %s %s",
                     scatter_opacity, scatter_opacity, y->first.c_str(), y->second.c_str());

            in_di->primitive = prim;
            DrawImage(in_img, in_di);
            GetImageException(in_img, &im_exception);
            if (im_exception.severity != UndefinedException) {
                CatchException(&im_exception);
                break;
            }

        }

    }

}

// Draw the legend and composite into the main map
//
// Make sure to call this as the LAST DRAWING OPTION or else it won't get
// the count right and various other things will go wrong.
//
// Text alignment SUCKS.  I don't like writing graphics code.
// This might screw up in some font situations, i'll deal with it whenever it
// comes to that.
int DrawLegendComposite(vector<gps_network *> in_nets, Image **in_img, 
                        DrawInfo **in_di) {
   // char pixdata[map_width][map_height + legend_height];
    unsigned int *pixdata;
    char prim[1024];
    ExceptionInfo im_exception;
    GetExceptionInfo(&im_exception);
    PixelPacket textclr;
    int tx_height;
    int cur_colpos = 5, cur_rowpos = map_height + 5;
    time_t curtime = time(0);
    int cur_column = 0;
    PixelPacket sqcol;
    // Width of the text column thats mandatory
    int text_colwidth = 0;
    // max val of each column
    map<int, int> max_col_map;
    int wepped_nets = 0, unwepped_nets = 0, default_nets = 0;

    for (map<mac_addr, gps_network *>::iterator dni = drawn_net_map.begin();
         dni != drawn_net_map.end(); ++dni) {
        gps_network *map_iter = dni->second;

        if (map_iter->wnet == NULL) {
            unwepped_nets++;
            continue;
        }

        if (map_iter->wnet->manuf_score == manuf_max_score) {
            default_nets++;
        } else if (map_iter->wnet->wep) {
            wepped_nets++;
        } else {
            unwepped_nets++;
        }
    }

    Image *leg_img = NULL;
    DrawInfo *leg_di = NULL;
    ImageInfo *leg_img_info = NULL;

    leg_img_info = CloneImageInfo((ImageInfo *) NULL);

    pixdata = (unsigned int *) malloc(sizeof(unsigned int) * 
                                      (map_width * (map_height + legend_height)));

    leg_img = ConstituteImage(map_width, map_height + legend_height, "RGB", CharPixel, 
                              pixdata, &im_exception);

    if (leg_img == (Image *) NULL) {
        fprintf(stderr, "FATAL: ImageMagick error:\n");
        MagickError(im_exception.severity, im_exception.reason,
                    im_exception.description);
        exit(0);
    }

    leg_di = CloneDrawInfo(leg_img_info, NULL);

    snprintf(prim, 1024, "stroke black fill-opacity 100%% rectangle 0,%d %d,%d",
             map_height, map_width, legend_height);
    leg_di->primitive = prim;
    DrawImage(leg_img, leg_di);
    GetImageException(leg_img, &im_exception);
    if (im_exception.severity != UndefinedException) {
        CatchException(&im_exception);
        return -1;
    }

    char text[1024];

    QueryColorDatabase("#FFFFFF", &textclr, &im_exception);
    if (im_exception.severity != UndefinedException) {
        CatchException(&im_exception);
        return -1;
    }
   
    leg_di->fill = textclr;
    leg_di->stroke = textclr;
    leg_di->font = strdup("courier");
    leg_di->pointsize = 14;
    leg_di->text_antialias = 1;

    // Figure out how many columns we're going to have...
    int ncolumns = 0;

    // Find the width of the everpresent text and then take the remaining area
    // and make it the right number of columns, based off their max width of 
    // contents

    // Test the standard text in col1
    snprintf(text, 1024, "Total networks  : %d\n", in_nets.size());
    text_colwidth = kismax(text_colwidth, IMStringWidth(text, leg_img, leg_di));

    snprintf(text, 1024, "Visible networks: %d\n", drawn_net_map.size());
    text_colwidth = kismax(text_colwidth, IMStringWidth(text, leg_img, leg_di));

    snprintf(text, 1024, "Map Created     : %.24s", ctime((const time_t *) &curtime));
    text_colwidth = kismax(text_colwidth, IMStringWidth(text, leg_img, leg_di));

    snprintf(text, 1024, "Map Coordinates : %f,%f @ scale %ld",
             map_avg_lat, map_avg_lon, map_scale);
    text_colwidth = kismax(text_colwidth, IMStringWidth(text, leg_img, leg_di));

    // Account for the margain
    text_colwidth += 5;

    // Now compare the sizes of the channel or color alloc
    int squaredim = IMStringHeight("0", leg_img, leg_di);

    if (draw_bounds || draw_range || draw_hull || draw_scatter || draw_center) {
        int curmax_colwidth = 0;
        if (color_coding == COLORCODE_WEP) {
            snprintf(text, 1024, "WEP Encrypted - %d (%2.2f%%)", wepped_nets,
                     ((double) wepped_nets / drawn_net_map.size()) * 100);
            curmax_colwidth = kismax(curmax_colwidth, 
                                     IMStringWidth(text, leg_img, leg_di) + 
                                     5 + squaredim);
            snprintf(text, 1024, "Unencrypted - %d (%2.2f%%)", unwepped_nets,
                     ((double) unwepped_nets / drawn_net_map.size()) * 100);
            curmax_colwidth = kismax(curmax_colwidth, 
                                     IMStringWidth(text, leg_img, leg_di) + 
                                     5 + squaredim);
            snprintf(text, 1024, "Factory Default - %d (%2.2f%%)", default_nets,
                     ((double) default_nets / drawn_net_map.size()) * 100);
            curmax_colwidth = kismax(curmax_colwidth, 
                                     IMStringWidth(text, leg_img, leg_di) + 5 + 
                                     squaredim);
            max_col_map[ncolumns] = curmax_colwidth;
            ncolumns++;
        } else if (color_coding == COLORCODE_CHANNEL) {
            curmax_colwidth = kismax(curmax_colwidth, squaredim * channelcolor_max);
            max_col_map[ncolumns] = curmax_colwidth;
            ncolumns++;
        } 

    }

    int power_step_skip = 1;
    if (draw_power && power_data != 0) {
        if (power_steps > 16)
            power_step_skip = power_steps / 16;

        int curmax_colwidth = squaredim * (power_steps / power_step_skip);
        max_col_map[ncolumns] = curmax_colwidth;
        ncolumns++;

    }

    // Now we know how wide we have to be...

    // Draw the first column of text, always have this
    snprintf(text, 1024, "Map Coordinates : %f,%f @ scale %ld",
             map_avg_lat, map_avg_lon, map_scale);
    tx_height = IMStringHeight(text, leg_img, leg_di);

    snprintf(prim, 1024, "text %d,%d \"%s\"",
             cur_colpos, cur_rowpos + (tx_height / 2), text);
    leg_di->text = text;
    leg_di->primitive = prim;
    DrawImage(leg_img, leg_di);
    GetImageException(leg_img, &im_exception);
    if (im_exception.severity != UndefinedException) {
        CatchException(&im_exception);
        return -1;
    }
    cur_rowpos += tx_height + 2;

    snprintf(text, 1024, "Total networks  : %d\n", in_nets.size());
    tx_height = IMStringHeight(text, leg_img, leg_di);

    snprintf(prim, 1024, "text %d,%d \"%s\"",
             cur_colpos, cur_rowpos + (tx_height / 2), text);
    leg_di->text = text;
    leg_di->primitive = prim;
    DrawImage(leg_img, leg_di);
    GetImageException(leg_img, &im_exception);
    if (im_exception.severity != UndefinedException) {
        CatchException(&im_exception);
        return -1;
    }
    cur_rowpos += tx_height + 2;
   

    snprintf(text, 1024, "Visible networks: %d\n", drawn_net_map.size());
    tx_height = IMStringHeight(text, leg_img, leg_di);

    snprintf(prim, 1024, "text %d,%d \"%s\"",
             cur_colpos, cur_rowpos + (tx_height / 2), text);
    leg_di->primitive = prim;
    DrawImage(leg_img, leg_di);
    GetImageException(leg_img, &im_exception);
    if (im_exception.severity != UndefinedException) {
        CatchException(&im_exception);
        return -1;
    }
    cur_rowpos += tx_height + 2;

    snprintf(text, 1024, "Map Created     : %.24s", ctime((const time_t *) &curtime));
    tx_height = IMStringHeight(text, leg_img, leg_di);

    snprintf(prim, 1024, "text %d,%d \"%s\"",
             cur_colpos, cur_rowpos + (tx_height / 2), text);
    leg_di->primitive = prim;
    DrawImage(leg_img, leg_di);
    GetImageException(leg_img, &im_exception);
    if (im_exception.severity != UndefinedException) {
        CatchException(&im_exception);
        return -1;
    }
    cur_rowpos += tx_height + 2;
  
    int avail_width = map_width - text_colwidth;
    
    // Draw the second column
    if ((draw_bounds || draw_range || draw_hull || draw_scatter || draw_center) &&
        (color_coding == COLORCODE_WEP || color_coding == COLORCODE_CHANNEL)) {
        cur_rowpos = map_height + 5;

        cur_colpos = text_colwidth + ((avail_width / ncolumns) * cur_column) +
            (((avail_width / ncolumns) / 2) - (max_col_map[cur_column] / 2));

        if (color_coding == COLORCODE_WEP) {
            // Draw the pretty colored squares
            QueryColorDatabase("#FF0000", &sqcol, &im_exception);
            if (im_exception.severity != UndefinedException) {
                CatchException(&im_exception);
                return -1;
            }
            snprintf(prim, 1024, "fill-opacity 100%% stroke-opacity 100%% "
                     "rectangle %d,%d %d,%d",
                     cur_colpos , cur_rowpos,
                     cur_colpos + squaredim, cur_rowpos + squaredim);
            leg_di->fill = sqcol;
            leg_di->primitive = prim;
            DrawImage(leg_img, leg_di);
            GetImageException(leg_img, &im_exception);
            if (im_exception.severity != UndefinedException) {
                fprintf(stderr, "FATAL: ImageMagick error:\n");
                MagickError(im_exception.severity, im_exception.reason,
                            im_exception.description);
                return -1;
            }
            cur_rowpos += squaredim + 2;

            QueryColorDatabase("#00FF00", &sqcol, &im_exception);
            if (im_exception.severity != UndefinedException) {
                CatchException(&im_exception);
                return -1;
            }
            snprintf(prim, 1024, "fill-opacity 100%% stroke-opacity 100%% "
                     "rectangle %d,%d %d,%d",
                     cur_colpos , cur_rowpos,
                     cur_colpos + squaredim, cur_rowpos + squaredim);
            leg_di->fill = sqcol;
            leg_di->primitive = prim;
            DrawImage(leg_img, leg_di);
            GetImageException(leg_img, &im_exception);
            if (im_exception.severity != UndefinedException) {
                fprintf(stderr, "FATAL: ImageMagick error:\n");
                MagickError(im_exception.severity, im_exception.reason,
                            im_exception.description);
                return -1;
            }
            cur_rowpos += squaredim + 2;

            QueryColorDatabase("#0000FF", &sqcol, &im_exception);
            if (im_exception.severity != UndefinedException) {
                CatchException(&im_exception);
                return -1;
            }
            snprintf(prim, 1024, "fill-opacity 100%% stroke-opacity 100%% "
                     "rectangle %d,%d %d,%d",
                     cur_colpos , cur_rowpos,
                     cur_colpos + squaredim, cur_rowpos + squaredim);
            leg_di->fill = sqcol;
            leg_di->primitive = prim;
            DrawImage(leg_img, leg_di);
            GetImageException(leg_img, &im_exception);
            if (im_exception.severity != UndefinedException) {
                fprintf(stderr, "FATAL: ImageMagick error:\n");
                MagickError(im_exception.severity, im_exception.reason,
                            im_exception.description);
                return -1;
            }

            // Go back and draw the text
            cur_rowpos = map_height + 5;
            cur_colpos += squaredim + 5;
            int tx_offset = (squaredim / 2) + (squaredim / 3);

            leg_di->fill = textclr;
            leg_di->stroke = textclr;

            snprintf(text, 1024, "WEP Encrypted - %d (%2.2f%%)", wepped_nets,
                     ((double) wepped_nets / drawn_net_map.size()) * 100);
            tx_height = IMStringHeight(text, leg_img, leg_di);

            snprintf(prim, 1024, "text %d,%d \"%s\"",
                     cur_colpos, cur_rowpos + tx_offset, text);
            leg_di->primitive = prim;
            DrawImage(leg_img, leg_di);
            GetImageException(leg_img, &im_exception);
            if (im_exception.severity != UndefinedException) {
                CatchException(&im_exception);
                return -1;
            }
            cur_rowpos += squaredim + 2;

            snprintf(text, 1024, "Unencrypted - %d (%2.2f%%)", unwepped_nets,
                     ((double) unwepped_nets / drawn_net_map.size()) * 100);
            tx_height = IMStringHeight(text, leg_img, leg_di);

            snprintf(prim, 1024, "text %d,%d \"%s\"",
                     cur_colpos, cur_rowpos + tx_offset, text);
            leg_di->primitive = prim;
            DrawImage(leg_img, leg_di);
            GetImageException(leg_img, &im_exception);
            if (im_exception.severity != UndefinedException) {
                CatchException(&im_exception);
                return -1;
            }
            cur_rowpos += squaredim + 2;

            snprintf(text, 1024, "Factory Default - %d (%2.2f%%)", default_nets,
                     ((double) default_nets / drawn_net_map.size()) * 100);
            tx_height = IMStringHeight(text, leg_img, leg_di);

            snprintf(prim, 1024, "text %d,%d \"%s\"",
                     cur_colpos, cur_rowpos + tx_offset, text);
            leg_di->primitive = prim;
            DrawImage(leg_img, leg_di);
            GetImageException(leg_img, &im_exception);
            if (im_exception.severity != UndefinedException) {
                CatchException(&im_exception);
                return -1;
            }
            cur_rowpos += squaredim + 2;
            
        } else if (color_coding == COLORCODE_CHANNEL) {
            // Draw the header
            leg_di->fill = textclr;
            leg_di->stroke = textclr;
            snprintf(text, 1024, "Channel Number");
            tx_height = IMStringHeight(text, leg_img, leg_di);
            snprintf(prim, 1024, "text %d,%d \"%s\"",
                     cur_colpos + (((channelcolor_max - 1) * squaredim) / 2) -
                     (IMStringWidth(text, leg_img, leg_di) / 2) + 4,
                     cur_rowpos + (IMStringHeight(text, leg_img, leg_di) / 2) + 3,
                     text);
            leg_di->primitive = prim;
            DrawImage(leg_img, leg_di);
            GetImageException(leg_img, &im_exception);
            if (im_exception.severity != UndefinedException) {
                fprintf(stderr, "FATAL: ImageMagick error:\n");
                MagickError(im_exception.severity, im_exception.reason,
                            im_exception.description);
                return -1;
            }
            cur_rowpos += tx_height + 2;

            // Draw each square in the channel graph sized to text
            for (int x = 0; x < channelcolor_max; x++) {
                QueryColorDatabase(channelcolors[x], &sqcol, &im_exception);
                if (im_exception.severity != UndefinedException) {
                    CatchException(&im_exception);
                    break;
                }

                snprintf(prim, 1024, "fill-opacity 100%% stroke-opacity 100%% "
                         "rectangle %d,%d %d,%d", 
                         cur_colpos + (x * squaredim), cur_rowpos,
                         cur_colpos + ((x+1) * squaredim), cur_rowpos + squaredim);
                //leg_di->stroke = sqcol;
                leg_di->fill = sqcol;
                leg_di->primitive = prim;
                DrawImage(leg_img, leg_di);
                GetImageException(leg_img, &im_exception);
                if (im_exception.severity != UndefinedException) {
                    fprintf(stderr, "FATAL: ImageMagick error:\n");
                    MagickError(im_exception.severity, im_exception.reason,
                                im_exception.description);
                    return -1;
                }
            }


            leg_di->fill = textclr;
            leg_di->stroke = textclr;
            snprintf(text, 1024, "1");
            snprintf(prim, 1024, "text %d,%d \"%s\"",
                     cur_colpos + (IMStringWidth(text, leg_img, leg_di) / 2),
                     cur_rowpos + squaredim + (IMStringHeight(text, leg_img, 
                                                              leg_di) / 2) + 3, 
                     text);
            leg_di->primitive = prim;
            DrawImage(leg_img, leg_di);
            GetImageException(leg_img, &im_exception);
            if (im_exception.severity != UndefinedException) {
                fprintf(stderr, "FATAL: ImageMagick error:\n");
                MagickError(im_exception.severity, im_exception.reason,
                            im_exception.description);
                return -1;
            }

            leg_di->fill = textclr;
            leg_di->stroke = textclr;
            snprintf(text, 1024, "%d", channelcolor_max);
            snprintf(prim, 1024, "text %d,%d \"%s\"",
                     cur_colpos +  
                     ((channelcolor_max - 1) * squaredim), 
                     cur_rowpos + squaredim + (IMStringHeight(text, leg_img, 
                                                              leg_di) / 2) + 3, 
                     text);
            leg_di->primitive = prim;
            DrawImage(leg_img, leg_di);
            GetImageException(leg_img, &im_exception);
            if (im_exception.severity != UndefinedException) {
                fprintf(stderr, "FATAL: ImageMagick error:\n");
                MagickError(im_exception.severity, im_exception.reason,
                            im_exception.description);
                return -1;
            }

        }
    }

    if (draw_power && power_data != 0) {
        cur_column++;
        cur_rowpos = map_height + 5;

        cur_colpos = text_colwidth + ((avail_width / ncolumns) * cur_column) +
            (((avail_width / ncolumns) / 2) - (max_col_map[cur_column] / 2));

        snprintf(text, 1024, "Signal Level");
        tx_height = IMStringHeight(text, leg_img, leg_di);
        
        int powerbarlen = ((power_steps / power_step_skip) * squaredim) + squaredim;
        leg_di->fill = textclr;
        leg_di->stroke = textclr;
        snprintf(prim, 1024, "text %d,%d \"%s\"",
                 cur_colpos + (powerbarlen / 2) - 
                 (IMStringWidth(text, leg_img, leg_di) / 2) + 4,
                 cur_rowpos + (tx_height / 2) + 3,
                 text);
        leg_di->primitive = prim;
        DrawImage(leg_img, leg_di);
        GetImageException(leg_img, &im_exception);
        if (im_exception.severity != UndefinedException) {
            fprintf(stderr, "FATAL: ImageMagick error:\n");
            MagickError(im_exception.severity, im_exception.reason,
                        im_exception.description);
            return -1;
        }
        cur_rowpos += tx_height + 2;

        int actual_pos = 0;
        for (int x = 0; x < power_steps; x += power_step_skip) {
            actual_pos++;

            QueryColorDatabase(power_colors[x], &sqcol, &im_exception);
            if (im_exception.severity != UndefinedException) {
                CatchException(&im_exception);
                break;
            }

            snprintf(prim, 1024, "fill-opacity 100%% stroke-opacity 100%% "
                     "rectangle %d,%d %d,%d", 
                     cur_colpos + (actual_pos * squaredim), cur_rowpos,
                     cur_colpos + ((actual_pos+1) * squaredim), cur_rowpos + squaredim);
            //leg_di->stroke = sqcol;
            leg_di->fill = sqcol;
            leg_di->primitive = prim;
            DrawImage(leg_img, leg_di);
            GetImageException(leg_img, &im_exception);
            if (im_exception.severity != UndefinedException) {
                fprintf(stderr, "FATAL: ImageMagick error:\n");
                MagickError(im_exception.severity, im_exception.reason,
                            im_exception.description);
                return -1;
            }

        }

        // Print the channel numbers and alloc name
        leg_di->fill = textclr;
        leg_di->stroke = textclr;
        snprintf(text, 1024, "<- Weaker");
        snprintf(prim, 1024, "text %d,%d \"%s\"",
                 cur_colpos + (IMStringWidth(text, leg_img, leg_di) / 3) - 3, 
                 cur_rowpos + squaredim + (IMStringHeight(text, 
                                                          leg_img, leg_di) / 2) + 3, 
                 text);
        leg_di->primitive = prim;
        DrawImage(leg_img, leg_di);
        GetImageException(leg_img, &im_exception);
        if (im_exception.severity != UndefinedException) {
            fprintf(stderr, "FATAL: ImageMagick error:\n");
            MagickError(im_exception.severity, im_exception.reason,
                        im_exception.description);
            return -1;
        }

        snprintf(text, 1024, "Stronger ->");
        snprintf(prim, 1024, "text %d,%d \"%s\"",
                 cur_colpos + ((actual_pos + 1) * squaredim) - 
                 IMStringWidth(text, leg_img, leg_di),  
                 cur_rowpos + squaredim + (IMStringHeight(text, 
                                                          leg_img, leg_di) / 2) + 3, 
                 text);
        leg_di->primitive = prim;
        DrawImage(leg_img, leg_di);
        GetImageException(leg_img, &im_exception);
        if (im_exception.severity != UndefinedException) {
            fprintf(stderr, "FATAL: ImageMagick error:\n");
            MagickError(im_exception.severity, im_exception.reason,
                        im_exception.description);
            return -1;
        }

    }

    snprintf(text, 1024, "Map generated by Kismet/GPSMap %d.%d.%d - "
             "http://www.kismetwireless.net", VERSION_MAJOR, 
             VERSION_MINOR, VERSION_TINY);
    QueryColorDatabase("#999999", &textclr, &im_exception);
    if (im_exception.severity != UndefinedException) {
        CatchException(&im_exception);
        return -1;
    }
    leg_di->fill = textclr;
    leg_di->stroke = textclr;
    leg_di->pointsize = 12;

    snprintf(prim, 1024, "text %d,%d \"%s\"",
             (map_width / 2) - (IMStringWidth(text, leg_img, leg_di) / 2),
             map_height + legend_height - (IMStringHeight(text, leg_img, leg_di) / 2),
             text);
    leg_di->primitive = prim;
    DrawImage(leg_img, leg_di);
    GetImageException(leg_img, &im_exception);
    if (im_exception.severity != UndefinedException) {
        fprintf(stderr, "FATAL: ImageMagick error:\n");
        MagickError(im_exception.severity, im_exception.reason,
                    im_exception.description);
        return -1;
    }

    // Now stick them together...  This doesn't take an exception and the return
    // type isn't documented, so lets just hope it does something.  Go IM!
    CompositeImage(leg_img, OverCompositeOp, *in_img, 0, 0);

    DestroyImage(*in_img);
    DestroyDrawInfo(*in_di);

    *in_img = leg_img;
    *in_di = leg_di;


    return 0;
}

int ShortUsage(char *argv) {
    printf("Usage: %s [OPTION] <GPS Files>\n", argv);
    printf("Try `%s --help' for more information.\n", argv);
    exit(1);
}

int Usage(char* argv, int ec = 1) { 
    printf("Usage: %s [OPTION] <GPS files>\n", argv);
    printf(
    //      12345678901234567890123456789012345678901234567890123456789012345678901234567890
           "  -h, --help                     What do you think you're reading?\n"
           "  -v, --verbose                  Verbose output while running\n"
           "  -g, --config-file <file>       Alternate config file\n"
           "  -o, --output <filename>        Image output file\n"
           "  -f, --filter <MAC list>        Comma-separated list of MACs to filter\n"
           "  -i, --invert-filter            Invert filtering (ONLY draw filtered MACs)\n"
           "  -F, --typefilter <Type list>   Comma-separated list of net types to filter\n"
           "  -I, --invert-typefilter        Invert type filtering\n"
           "  -z, --threads <num>            Number of simultaneous threads used for\n"
           "                                  complex operations [Default: 1]\n"
           "  -S, --map-source <#>           Source to download maps from [Default: 0]\n"
           "                                  0 MapBlast (vector)\n"
           "                                  1 MapPoint (vector)\n"
           "                                  2 TerraServer (photo)\n"
           "                                  3 Tiger US Census (vector)\n"
           "  -D, --keep-gif                 Keep the downloaded map\n"
           "  -V, --version                  GPSMap version\n"
           "\nImage options\n"
           "  -c, --coords <lat,lon>         Force map center at lat,lon\n"
           "  -s, --scale <s>                Force map scale at s\n"
           "  -m, --user-map <map>           Use custom map instead of downloading\n"
           "  -d, --map-size <x,y>           Download map at size x,y\n"
           "  -n, --network-colors <c>       Network drawing colors [Default: 0]\n"
           "                                  0 is random colors\n"
           "                                  1 is color based on WEP status\n"
           "                                  2 is color based on network channel\n"
           "  -G, --no-greyscale             Don't convert map to greyscale\n"
           "  -M, --metric                   Fetch metric-titled map\n"
           "  -O, --offset <x,y>             Offset drawn features by x,y pixels\n"
           "\nDraw options\n"
           "  -t, --draw-track               Draw travel track\n"
           "  -Y, --draw-track-width <w>     travel track width [Default: 3] \n"
           "  -b, --draw-bounds              Draw network bounding box\n"
           "  -r, --draw-range               Draw estimaged range circles\n"
           "  -R, --draw-range-opacity <o>   Range circle opacity [Default: 70]\n"
           "  -u, --draw-hull                Draw convex hull of data points\n"
           "  -U, --draw-hull-opacity <o>    Convex hull opacity [Default: 70]\n"
           "  -a, --draw-scatter             Draw scatter plot of data points\n"
           "  -A, --draw-scatter-opacity <o> Scatter plot opacity [Default: 100]\n"
           "  -B, --draw-scatter-size <s>    Draw scatter at radius size <s> [Default: 2]\n"
           "  -Z, --draw-power-zoom          Power based scatter plot range for scaling colors [Default: 0]\n"
           "                                 0 determines upper limit based on max observed for network\n"
           "                                 1-255 is user defined upper limit\n"
           "  -p, --draw-power               Draw interpolated network power\n"
           "  -P, --draw-power-opacity <o>   Interpolated power opacity [Default: 70]\n"
           "  -Q, --draw-power-res <res>     Interpolated power resolution [Default: 5]\n"
           "  -q, --draw-power-colors <c>    Interpolated power color set [Default: 0]\n"
           "                                  0 is a ramp though RGB colorspace (12 colors)\n"
           "                                  1 is the origional color set (10 colors)\n"
           "                                  2 is weathermap radar style (9 colors)\n"
           "  -e, --draw-center              Draw dot at center of network range\n"
           "  -E, --draw-center-opacity <o>  Center dot opacity [Default: 100]\n"
           "  -H, --draw-center-size <s>     Center dot at radius size <s> [Default: 2]\n"
           "  -l, --draw-labels <list>       Draw network labels, comma-seperated list\n"
           "                                  (bssid, name)\n"
           "  -L, --draw-label-orient <o>    Label orientation [Default: 7]\n"
           "                                  0       1       2\n"
           "                                  3       4       5\n"
           "                                  6       7       8\n"
           "  -k, --draw-legend              Draw map legend\n"
           "  -T, --feature-order <order>    String representing the order map features\n"
           "                                  are drawn [Default: 'ptbrhscl']\n"
           "                                  p: interpolated power\n"
           "                                  t: tracks\n"
           "                                  b: bounds\n"
           "                                  r: range circles\n"
           "                                  h: convex hulls\n"
           "                                  s: scatter plot\n"
           "                                  c: center dot\n"
           "                                  l: labels\n"
          );
    exit(ec);
}

char *exec_name;

int main(int argc, char *argv[]) {
    char* exec_name = argv[0];

    char mapname[1024];
    char mapoutname[1024];

    bool metric = false;

    char *ap_manuf_name = NULL, *client_manuf_name = NULL;
    FILE *manuf_data;

    static struct option long_options[] = {   /* options table */
           {"help", no_argument, 0, 'h'},
           {"verbose", no_argument, 0, 'v'},
           {"config-file", required_argument, 0, 'g'},
           {"map-source", required_argument, 0, 'S'},
           {"output", required_argument, 0, 'o'},
           {"filter", required_argument, 0, 'f'},
           {"invert-filter", no_argument, 0, 'i'},
           {"typefilter", required_argument, 0, 'F'},
           {"invert-typefilter", required_argument, 0, 'I'},
           {"threads", required_argument, 0, 'z'},
           {"keep-gif", no_argument, 0, 'D'},
           {"version", no_argument, 0, 'V'},
           {"coords", required_argument, 0, 'c'},
           {"scale", required_argument, 0, 's'},
           {"user-map", required_argument, 0, 'm'},
           {"map-size", required_argument, 0, 'd'},
           {"network-colors", required_argument, 0, 'n'},
           {"no-greyscale", no_argument, 0, 'G'},
           {"metric", no_argument, 0, 'M'},
           {"offset", required_argument, 0, 'O'},
           {"draw-track", no_argument, 0, 't'},
           /*
           {"draw-track-opacity", required_argument, 0, 'T'},
           */
           {"draw-track-width", required_argument, 0, 'Y'},
           {"draw-bounds", no_argument, 0, 'b'},
           {"draw-range", no_argument, 0, 'r'},
           {"draw-range-opacity", required_argument, 0, 'R'},
           {"draw-hull", no_argument, 0, 'u'},
           {"draw-hull-opacity", required_argument, 0, 'U'},
           {"draw-scatter", no_argument, 0, 'a'},
           {"draw-scatter-opacity", required_argument, 0, 'A'},
           {"draw-scatter-size", required_argument, 0, 'B'},
           {"draw-power-zoom", required_argument, 0, 'Z'}, // ATR - option for scaling with power based scatter plots
           {"draw-power", no_argument, 0, 'p'},
           {"draw-power-opacity", required_argument, 0, 'P'},
           {"draw-power-res", required_argument, 0, 'Q'},
           {"draw-power-colors", required_argument, 0, 'q'},
           {"draw-center", no_argument, 0, 'e'},
           {"draw-center-opacity", required_argument, 0, 'E'},
           {"draw-center-size", required_argument, 0, 'H'},
           {"draw-labels", required_argument, 0, 'l'},
           {"draw-label-orient", required_argument, 0, 'L'},
           {"draw-legend", no_argument, 0, 'k'},
           {"feature-order", required_argument, 0, 'T'},
           { 0, 0, 0, 0 }
    };
    int option_index;

    bool usermap = false, useroutmap = false, filemap = false;

    power_steps = power_steps_Math;
    power_colors = powercolors_Math;

    float user_lat = 0, user_lon = 0;
    bool user_latlon = false;
    long user_scale = 0;

    int usersize = 0;

    long fetch_scale = 0;

    sample_points = 0;

    char *configfile = NULL;

    vector<string> opttok;
    mac_addr fm;
    string toklow;
    wireless_network_type wt;

    while(1) {
        int r = getopt_long(argc, argv,
                            "hvg:S:o:f:iF:Iz:DVc:s:m:d:n:GMO:tY:brR:uU:aA:B:pP:Z:q:Q:eE:H:l:L:kT:",
                            long_options, &option_index);

        if (r < 0) break;

        switch(r) {
        case 'h':
            Usage(exec_name, 0);
            break;
        case 'v':
            verbose = true;
            break;
        case 'g':
            configfile = optarg;
            break;
        case 'o':
            snprintf(mapoutname, 1024, "%s", optarg);
            useroutmap = true;
            break;
        case 'f':
            opttok = StrTokenize(optarg, ",");
            for (unsigned int tv = 0; tv < opttok.size(); tv++) {
                fm = opttok[tv].c_str();
                if (fm.error) {
                    ShortUsage(exec_name);
                }

                filter_map.insert(fm, 1);
            }
            break;
        case 'F':
            opttok = StrTokenize(optarg, ",");
            for (unsigned int tv = 0; tv < opttok.size(); tv++) {
                toklow = StrLower(opttok[tv]);

                if (toklow == "ap" || toklow == "accesspoint")
                    wt = network_ap;
                else if (toklow == "adhoc" || toklow == "ad-hoc")
                    wt = network_adhoc;
                else if (toklow == "probe")
                    wt = network_probe;
                else if (toklow == "turbocell" || toklow == "turbo-cell")
                    wt = network_turbocell;
                else if (toklow == "data")
                    wt = network_data;
                else {
                    fprintf(stderr, "Invalid network type in filter, '%s'\n",
                            opttok[tv].c_str());
                    ShortUsage(exec_name);
                }

                type_filter_map[wt] = 1;
            }
            break;
        case 'S':
            if (sscanf(optarg, "%d", &mapsource) != 1 || mapsource < 0 || mapsource > 3) {
                fprintf(stderr, "Invalid map source.\n");
                ShortUsage(exec_name);
            }
            break;
        case 'i':
            invert_filter = 1;
            break;
        case 'I':
            invert_type_filter = 1;
             break;
        case 'z':
#ifdef HAVE_PTHREAD
            if (sscanf(optarg, "%d", &numthreads) != 1 || numthreads < 1) {
                fprintf(stderr, "Invalid number of threads.\n");
                ShortUsage(exec_name);
            }
#else
            fprintf(stderr, "PThread support was not compiled.\n");
            ShortUsage(exec_name);
#endif
            break;
        case 'D':
            keep_gif = true;
            break;
        case 'V':
            printf("GPSMap v%i.%i.%i\n", VERSION_MAJOR, VERSION_MINOR, VERSION_TINY);
            exit(0);
            break;
        case 'c':
            if (sscanf(optarg, "%f,%f", &user_lat, &user_lon) != 2) {
                fprintf(stderr, "Invalid custom map coordinates.\n");
                ShortUsage(exec_name);
            }
            user_latlon = true;
            break;
        case 's':
            if (sscanf(optarg, "%ld", &user_scale) != 1) {
                fprintf(stderr, "Invalid custom map scale.\n");
                ShortUsage(exec_name);
            }
            break;
        case 'm':
            snprintf(mapname, 1024, "%s", optarg);
            usermap = true;
            break;
       	case 'd':
            if (sscanf(optarg, "%d,%d", &map_width, &map_height) != 2 || map_width < 0 || map_height < 0) {
                fprintf(stderr, "Invalid custom map size.\n");
                ShortUsage(exec_name);
            }
            usersize = 1;
            break;
        case 'n':
            if (sscanf(optarg, "%d", &color_coding) !=1 || color_coding < 0 || color_coding > 2) {
                fprintf(stderr, "Invalid network color set\n");
                ShortUsage(exec_name);
            }
            break;
        case 'G':
            convert_greyscale = false;
            break;
        case 'M':
            metric = true;
            break;
        case 'O':
            if (sscanf(optarg, "%d,%d", &draw_x_offset, &draw_y_offset) != 2) {
                fprintf(stderr, "Invalid drawing offset.\n");
                ShortUsage(exec_name);
            }
            break;
        case 't':
            draw_track = true;
            break;
            /*
        case 'T':
            if (sscanf(optarg, "%d", &track_opacity) != 1 || track_opacity < 0 || track_opacity > 100) {
                fprintf(stderr, "Invalid track opacity.\n");
                Usage(exec_name);
            }
            break;
            */            
        case 'Y':             /* Ge0 was here - this could very well break crap */
            if (sscanf(optarg, "%d", &track_width) != 1 || track_width <= 0) {
                fprintf(stderr, "Invalid track width.\n");
                ShortUsage(exec_name);
            }
            break;
        case 'b':
            draw_bounds = true;
            break;
        case 'r':
            draw_range = true;
            break;
        case 'R':
            if (sscanf(optarg, "%d", &range_opacity) != 1 || range_opacity < 0 || range_opacity > 100) {
                fprintf(stderr, "Invalid range opacity.\n");
                ShortUsage(exec_name);
            }
            break;
        case 'u':
            draw_hull = true;
            break;
        case 'U':
            if (sscanf(optarg, "%d", &hull_opacity) != 1 || hull_opacity < 0 || hull_opacity > 100) {
                fprintf(stderr, "Invalid convex hull opacity.\n");
                ShortUsage(exec_name);
            }
            break;
        case 'a':
            draw_scatter = true;
            break;
        case 'A':
            if (sscanf(optarg, "%d", &scatter_opacity) != 1 || scatter_opacity < 0 || scatter_opacity > 100) {
                fprintf(stderr, "Invalid scatter plot opacity.\n");
                ShortUsage(exec_name);
            }
            break;
        case 'B':
            if (sscanf(optarg, "%d", &scatter_resolution) != 1 || scatter_resolution < 1) {
                fprintf(stderr, "Invalid scatter plot size.\n");
                ShortUsage(exec_name);
            }
            break;
        case 'p':
            draw_power = true;
            break;
        case 'P':
            if (sscanf(optarg, "%d", &power_opacity) != 1 || power_opacity < 0 || power_opacity > 100) {
                fprintf(stderr, "Invalid interpolated power opacity.\n");
                ShortUsage(exec_name);
            }
            break;
        case 'Q':
            if (sscanf(optarg, "%d", &power_resolution) != 1 || power_resolution < 1) {
                fprintf(stderr, "Invalid interpolated power resolution.\n");
                ShortUsage(exec_name);
            }
            break;
        case 'Z':
            if (sscanf(optarg, "%d", &power_zoom) != 1 || power_zoom > 255 || power_zoom < 0) {
                fprintf(stderr, "Invalid scatter power zoom.\n");
                ShortUsage(exec_name);
            }
            break;
        case 'q':
            {
                int icolor;
                if (sscanf(optarg, "%d", &icolor) !=1 || icolor < 0 || icolor > 3) {
                    fprintf(stderr, "Invalid interpolated power color set\n");
                    ShortUsage(exec_name);
                }

                // ATR - set vars for scatter plot
                scatter_power = 1;

                powercolor_index = icolor;

                if (icolor == 0) {
                    power_steps = power_steps_Orig;
                    power_colors = powercolors_Orig;
                } else if (icolor == 2) {
                    power_steps = power_steps_Radar;
                    power_colors = powercolors_Radar;
                } else if (icolor == 3) {
                    power_steps = power_steps_Blue;
                    power_colors = powercolors_Blue;
                }
            }
            break;
        case 'e':
            draw_center = true;
            break;
        case 'E':
            if (sscanf(optarg, "%d", &center_opacity) != 1 || center_opacity < 0 || center_opacity > 100) {
                fprintf(stderr, "Invalid center dot opacity.\n");
                ShortUsage(exec_name);
            }
            break;
        case 'H':
            if (sscanf(optarg, "%d", &center_resolution) != 1 || center_resolution < 1) {
                fprintf(stderr, "Invalid center dot size.\n");
                ShortUsage(exec_name);
            }
            break;
        case 'l':
            network_labels = optarg;
            draw_label = 1;
            break;
        case 'L':
            if (sscanf(optarg, "%d", &label_orientation) != 1 || label_orientation < 0 ||
                label_orientation > 8) {
                fprintf(stderr, "Invalid label orientation.\n");
                ShortUsage(exec_name);
            }
            break;
        case 'k':
            draw_legend = true;
            break;
        case 'T':
            draw_feature_order = optarg;
            break;
        default:
            ShortUsage(exec_name);
            break;
        }
    }

    if (verbose) {
        // start up messages
    }

    if (draw_power == 0 && draw_track == 0 && draw_bounds == 0 && draw_range == 0 &&
        draw_hull == 0 && draw_scatter == 0 && draw_center == 0 && draw_label == 0) {
        fprintf(stderr, "FATAL:  No drawing methods requested.\n");
        ShortUsage(exec_name);
    }

    if ((map_width > 1280 || map_height > 1024) && mapsource == MAPSOURCE_MAPBLAST) {
        fprintf(stderr, "WARNING:  Maximum Mapblast image size is 1024x1280.  Adjusting.\n");
        map_width = 1024;
        map_height = 1280;
    }

    // sanity checks

    // no dump files
    if (optind == argc) {
        fprintf(stderr, "FATAL:  Must provide at least one gps file.\n");
        ShortUsage(exec_name);
    }

    ConfigFile conf;

    // If we haven't gotten a command line config option...
    if (configfile == NULL) {
        configfile = (char *) malloc(1024*sizeof(char));
        snprintf(configfile, 1024, "%s/%s", SYSCONF_LOC, config_base);
    }

    // Parse the config and load all the values from it and/or our command
    // line options.  This is a little soupy but it does the trick.
    if (conf.ParseConfig(configfile) < 0) {
        fprintf(stderr, "WARNING:  Couldn't open config file '%s'.  Will continue anyway, but MAC filtering and manufacturer detection will be disabled\n",
                configfile);
        configfile = NULL;
    }

    if (configfile != NULL) {
        if (conf.FetchOpt("ap_manuf") != "") {
            ap_manuf_name = strdup(conf.FetchOpt("ap_manuf").c_str());
        } else {
            fprintf(stderr, "WARNING:  No ap_manuf file specified, AP manufacturers and defaults will not be detected.\n");
        }

        if (conf.FetchOpt("client_manuf") != "") {
            client_manuf_name = strdup(conf.FetchOpt("client_manuf").c_str());
        } else {
            fprintf(stderr, "WARNING:  No client_manuf file specified.  Client manufacturers will not be detected.\n");
        }

    }

    // Catch a null-draw condition
    if (invert_filter == 1 && filter_map.size() == 0) {
        fprintf(stderr, "FATAL:  Inverse filtering requested but no MAC's given to draw.\n");
        exit(1);
    }

    if (mapsource == MAPSOURCE_TERRA) {
    // It's way too much of a kludge to muck with munging the scale around
        if ((user_scale < 10) || (user_scale > 16)) {
            fprintf(stderr, "FATAL: You must provide a scale with the -s option that is from 10 to 16\n");
            exit(0);
        }
        fetch_scale = user_scale;
        map_scale = user_scale = terrascales[(user_scale - 10)];
    }

    if (ap_manuf_name != NULL) {
        char pathname[1024];

        if (strchr(ap_manuf_name, '/') == NULL)
            snprintf(pathname, 1024, "%s/%s", SYSCONF_LOC, ap_manuf_name);
        else
            snprintf(pathname, 1024, "%s", ap_manuf_name);

        if ((manuf_data = fopen(pathname, "r")) == NULL) {
            fprintf(stderr, "WARNING:  Unable to open '%s' for reading (%s), AP manufacturers and defaults will not be detected.\n",
                    pathname, strerror(errno));
        } else {
            fprintf(stderr, "Reading AP manufacturer data and defaults from %s\n", pathname);
            ap_manuf_map = ReadManufMap(manuf_data, 1);
            fclose(manuf_data);
        }

        free(ap_manuf_name);
    }

    if (client_manuf_name != NULL) {
        char pathname[1024];

        if (strchr(client_manuf_name, '/') == NULL)
            snprintf(pathname, 1024, "%s/%s", SYSCONF_LOC, client_manuf_name);
        else
            snprintf(pathname, 1024, "%s", client_manuf_name);

        if ((manuf_data = fopen(pathname, "r")) == NULL) {
            fprintf(stderr, "WARNING:  Unable to open '%s' for reading (%s), client manufacturers and defaults will not be detected.\n",
                    pathname, strerror(errno));
        } else {
            fprintf(stderr, "Reading client manufacturer data and defaults from %s\n", pathname);
            client_manuf_map = ReadManufMap(manuf_data, 1);
            fclose(manuf_data);
        }

        free(client_manuf_name);
    }

    // Initialize stuff
    num_tracks = 0;
//    memset(&global_map_avg, 0, sizeof(gps_network));

#ifdef HAVE_PTHREAD
    // Build the threads
    mapthread = new pthread_t[numthreads];//(pthread_t *) malloc(sizeof(pthread_t) * numthreads);
    pthread_mutex_init(&power_lock, NULL);
    pthread_mutex_init(&print_lock, NULL);
    pthread_mutex_init(&power_pos_lock, NULL);
#endif

    // Imagemagick stuff
    Image *img = NULL;
    ImageInfo *img_info;
    DrawInfo *di;

    InitializeMagick(*argv);
    GetExceptionInfo(&im_exception);
    img_info = CloneImageInfo((ImageInfo *) NULL);

    if (optind == argc) {
        fprintf(stderr, "FATAL:  Must provide at least one dump file.\n");
        ShortUsage(exec_name);
    }

    for (int x = optind; x < argc; x++) {
        if (ProcessGPSFile(argv[x]) < 0) {
            fprintf(stderr, "WARNING:  Unrecoverable error processing GPS data file \"%s\", skipping.\n",
                    argv[x]);
            //exit(1);
        }
    }

    fprintf(stderr, "Processing %d sample points.\n",
            sample_points);

    map_avg_lat = (double) (global_map_avg.min_lat + global_map_avg.max_lat) / 2;
    map_avg_lon = (double) (global_map_avg.min_lon + global_map_avg.max_lon) / 2;

    // Fit the whole map
    map_scale = BestMapScale(global_map_avg.min_lat, global_map_avg.min_lon,
                             global_map_avg.max_lat, global_map_avg.max_lon);

    fprintf(stderr, "Map image scale: %ld\n", map_scale);
    fprintf(stderr, "Minimum Corner (lat/lon): %f x %f\n", global_map_avg.min_lat,
            global_map_avg.min_lon);
    fprintf(stderr, "Maximum Corner (lat/lon): %f x %f\n", global_map_avg.max_lat,
            global_map_avg.max_lon);
    fprintf(stderr, "Map center (lat/lon): %f x %f\n", map_avg_lat, map_avg_lon);

    if (usermap && user_scale == 0 && user_lat == 0 &&
        user_lon == 0 && usersize == 0) {
        float filelat, filelon;
        long filescale;
        int filewidth, fileheight;

        if (sscanf(mapname, "map_%f_%f_%ld_%d_%d.gif",
                   &filelat, &filelon, &filescale, &filewidth, &fileheight) == 5) {
            user_lat = filelat;
            user_lon = filelon;
            user_scale = filescale;
            map_width = filewidth;
            map_height = fileheight;
        }
    }

    if (user_scale != 0) {
        fprintf(stderr, "Overriding with user scale: %ld\n", user_scale);
        map_scale = user_scale;
    }

    if (user_lat != 0) {
        fprintf(stderr, "Overriding with user map center (lat/lon): %f x %f\n", user_lat, user_lon);

        map_avg_lat = user_lat;
        map_avg_lon = user_lon;
    }

    if (map_scale == 0) {
        fprintf(stderr, "Unable to find a map at any scale to fit the data.\n");
        exit(0);
    }

    if (!usermap) {
        snprintf(mapname, 1024, "map_%f_%f_%ld_%d_%d.gif", map_avg_lat, map_avg_lon,
                 map_scale, map_width, map_height);
    }

    if (useroutmap == false)
        snprintf(mapoutname, 1024, "map_%f_%f_%ld_%d_%d.png", map_avg_lat, map_avg_lon,
                 map_scale, map_width, map_height);

    printf("Loading map into Imagemagick structures.\n");
    strcpy(img_info->filename, mapname);
    img = ReadImage(img_info, &im_exception);

    if (img == (Image *) NULL) {
        if (usermap) {
            printf("Unable to load '%s'\n", mapname);
            exit(1);
        }

        char url[1024];

        if (mapsource == MAPSOURCE_TERRA) {
            snprintf(url, 1024, url_template_ts, map_avg_lat, map_avg_lon, fetch_scale,
                     map_width, map_height);
        } else if (mapsource == MAPSOURCE_MAPBLAST) {
            fetch_scale = map_scale;
            snprintf(url, 1024, url_template_mb, map_avg_lat, map_avg_lon, fetch_scale,
                     map_width, map_height, metric ? "&DU=KM" : "");
        } else if (mapsource == MAPSOURCE_MAPPOINT) {
            fetch_scale = (long) (map_scale / 1378.6);
            snprintf(url, 1024, url_template_mp, map_avg_lat, map_avg_lon, fetch_scale,
                     map_width, map_height);
        } else if (mapsource == MAPSOURCE_TIGER) {
            snprintf(url, 1024, url_template_ti, map_avg_lat, map_avg_lon, (map_scale / 300000.0),
                     map_width, map_height);
        }

        printf("Map url: %s\n", url);
        printf("Fetching map...\n");

        char geturl[1024];
        snprintf(geturl, 1024, download_template, url, mapname);
        system(geturl);

        printf("Loading map into Imagemagick structures.\n");
        strcpy(img_info->filename, mapname);
        img = ReadImage(img_info, &im_exception);

        if (img == (Image *) NULL) {
            fprintf(stderr, "FATAL:  ImageMagick error:\n");
            MagickError(im_exception.severity, im_exception.reason, im_exception.description);
            exit(0);
        }
    } else {
        filemap = true;
    }

    strcpy(img_info->filename, mapoutname);
    strcpy(img->filename, mapoutname);

    // Convert it to greyscale and then back to color
    if (convert_greyscale) {
        fprintf(stderr, "Converting map to greyscale.\n");
        SetImageType(img, GrayscaleType);
        SetImageType(img, TrueColorType);
    }

    fprintf(stderr, "Calculating network coordinates and statistics...\n");
    ProcessNetData(verbose);

    fprintf(stderr, "Assigning network colors...\n");
    AssignNetColors();

    // Build a vector.  This can become a selected list in the future.
    vector<gps_network *> gpsnetvec;
    for (map<string, gps_network *>::iterator x = bssid_gpsnet_map.begin();
         x != bssid_gpsnet_map.end(); ++x) {

        // Skip filtered
        if (x->second->filtered)
            continue;

        gpsnetvec.push_back(x->second);
    }

    fprintf(stderr, "Plotting %d networks...\n", gpsnetvec.size());

    di = CloneDrawInfo(img_info, NULL);
    for (unsigned int x = 0; x < draw_feature_order.length(); x++) {
        switch (draw_feature_order[x]) {
        case 'p':
            if (draw_power && power_data == 0) {
                fprintf(stderr, "ERROR:  Interpolated power drawing requested, but none of the GPS datafiles being\n"
                        "processed have power data.  Not doing interpolated graphing.\n");
            } else if (draw_power) {
                fprintf(stderr, "Drawing network power interpolations...\n");
                DrawNetPower(gpsnetvec, img, di);
            }
            break;
        case 't':
            if (draw_track) {
                fprintf(stderr, "Drawing track coordinates, width: %d...\n", track_width);
                DrawNetTracks(img, di);
            }
            break;
        case 'b':
            if (draw_bounds) {
                fprintf(stderr, "Calculating and drawing bounding rectangles...\n");
                DrawNetBoundRects(gpsnetvec, img, di, 0);
            }
            break;
        case 'r':
            if (draw_range) {
                fprintf(stderr, "Calculating and drawing network circles...\n");
                DrawNetCircles(gpsnetvec, img, di);
            }
            break;
        case 'h':
            if (draw_hull) {
                fprintf(stderr, "Calculating and drawing network hulls...\n");
                DrawNetHull(gpsnetvec, img, di);
            }
            break;
        case 's':
            if (draw_scatter) {
                fprintf(stderr, "Drawing scatter plot, dot size %d...\n", scatter_resolution);
                DrawNetScatterPlot(gpsnetvec, img, di);
            }
            break;
        case 'c':
            if (draw_center) {
                fprintf(stderr, "Drawing center dot, size %d...\n", center_resolution);
                DrawNetCenterDot(gpsnetvec, img, di);
            }
            break;
        case 'l':
            if (draw_label) {
                fprintf(stderr, "Labeling networks...\n");
                DrawNetCenterText(gpsnetvec, img, di);
            }
            break;
        default:
            fprintf(stderr, "WARNING:  Unknown feature '%c' in requested order.  Skipping.\n",
                    draw_feature_order[x]);
            break;
        };
    }

    // Make sure our DI has a clean primitive, since all of the other assignments were
    // local variables
    di->text = strdup("");
    di->primitive = strdup("");

    // Draw the legend if we're going to...  And of course since it has to be
    // annoying, it needs to be able to modify img itself so it's a pointer to
    // a pointer.
    if (draw_legend) {
        fprintf(stderr, "Drawing legend...\n");
        DrawLegendComposite(gpsnetvec, &img, &di);

        // Clean up and set our new image out values
        di->text = strdup("");
        di->primitive = strdup("");
        img_info = CloneImageInfo((ImageInfo *) NULL);
        strcpy(img_info->filename, mapoutname);
        strcpy(img->filename, mapoutname);
    }

    WriteImage(img_info, img);

    DestroyDrawInfo(di);
    DestroyImage(img);

    DestroyMagick();

#ifdef HAVE_PTHREAD
    delete[] mapthread;
#endif

    if (!keep_gif && !usermap && !filemap) {
        fprintf(stderr, "Unlinking downloaded map.\n");
        unlink(mapname);
    }

}

#endif

