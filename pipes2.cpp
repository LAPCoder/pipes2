// g++ pipes2.cpp -o pipes2 -Wall -Wextra -fuse-ld=lld -Wshadow -g -fsanitize=address,undefined -O3 -std=c++20

#include <chrono>
#include <thread>
#include <vector>
#include <cstdlib>
#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <array>
#include <cstring>

////////////////////////////////////////////////////////////////////////////////

// Thanks cross-platform compat...
// (thanks ProjectPhysX)
#if defined(_WIN32)
#	define WIN32_LEAN_AND_MEAN
#	define VC_EXTRALEAN
#	include <Windows.h>
#elif defined(__linux__) || defined(__APPLE__)
#	include <sys/ioctl.h>
#endif // Windows/Linux

void get_terminal_size(int &width, int &height)
{
#if defined(_WIN32)
	CONSOLE_SCREEN_BUFFER_INFO csbi;
	GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi);
	width = (int)(csbi.srWindow.Right-csbi.srWindow.Left+1);
	height = (int)(csbi.srWindow.Bottom-csbi.srWindow.Top+1);
#elif defined(__linux__) || defined(__APPLE__)
	struct winsize w;
	ioctl(fileno(stdout), TIOCGWINSZ, &w);
	width = (int)(w.ws_col);
	height = (int)(w.ws_row);
#endif // Windows/Linux
}

// Converts HSV hue (brightness and value are 255) to RGB color
// You need to use r=LUT[3*i+0] g=LUT[3*i+1] b=[3*i+2]
consteval std::array<uint8_t,256 * 3> make_hue_LUT()
{
	std::array<uint8_t,256 * 3> lut;
	uint8_t *r = lut.begin(),
	        *g = r + 1,
	        *b = r + 2;

	for (uint_fast16_t i = 0; i < 256; i++) {
	    uint8_t sector = i / 43;
		uint8_t offset = i - sector * 43;

		uint8_t t = ((uint16_t)offset * 255 + 21) / 43;

		switch (sector)	{
			case 0: *r = 255;     *g = t;       *b = 0;       break;
			case 1: *r = 255 - t; *g = 255;     *b = 0;       break;
			case 2: *r = 0;       *g = 255;     *b = t;       break;
			case 3: *r = 0;       *g = 255 - t; *b = 255;     break;
			case 4: *r = t;       *g = 0;       *b = 255;     break;
			default:*r = 255;     *g = 0;       *b = 255 - t; break;
		}

		r+=3, g+=3, b+=3;
	}
	return lut;
}

static constexpr std::array<uint8_t,256 * 3> HUE_LUT = make_hue_LUT();

/* DEPRECATED: slow
// Top left has coords 0;0. XY order (width, height)
// Use it like printf
inline int print_at(int w, int h, const char *fmt, ...)
{
	char combined_fmt[512];
	snprintf(
		combined_fmt, sizeof(combined_fmt),
		"\033[%i;%iH%s", h - 1, w - 1, fmt);
	
	va_list args;
	va_start(args, fmt);
	int r = vprintf(combined_fmt, args);
	va_end(args);

	return r;
}
// Same as print_at but with color
inline int print_at_rgb(int w, int h, const char rgb[3], const char *fmt, ...)
{
	char combined_fmt[512];
	snprintf(
		combined_fmt, sizeof(combined_fmt),
		"\033[%i;%iH\033[38;2;%hhu;%hhu;%hhum%s",
		     h-1,w-1,  rgb[0],rgb[1],rgb[2], fmt);
	
	va_list args;
	va_start(args, fmt);
	int r = vprintf(combined_fmt, args);
	va_end(args);

	return r;
}*/

static constexpr char NUM_TO_CHAR_LUT[256][4] = {
	"000","001","002","003","004","005","006","007","008","009",
	"010","011","012","013","014","015","016","017","018","019",
	"020","021","022","023","024","025","026","027","028","029",
	"030","031","032","033","034","035","036","037","038","039",
	"040","041","042","043","044","045","046","047","048","049",
	"050","051","052","053","054","055","056","057","058","059",
	"060","061","062","063","064","065","066","067","068","069",
	"070","071","072","073","074","075","076","077","078","079",
	"080","081","082","083","084","085","086","087","088","089",
	"090","091","092","093","094","095","096","097","098","099",
	"100","101","102","103","104","105","106","107","108","109",
	"110","111","112","113","114","115","116","117","118","119",
	"120","121","122","123","124","125","126","127","128","129",
	"130","131","132","133","134","135","136","137","138","139",
	"140","141","142","143","144","145","146","147","148","149",
	"150","151","152","153","154","155","156","157","158","159",
	"160","161","162","163","164","165","166","167","168","169",
	"170","171","172","173","174","175","176","177","178","179",
	"180","181","182","183","184","185","186","187","188","189",
	"190","191","192","193","194","195","196","197","198","199",
	"200","201","202","203","204","205","206","207","208","209",
	"210","211","212","213","214","215","216","217","218","219",
	"220","221","222","223","224","225","226","227","228","229",
	"230","231","232","233","234","235","236","237","238","239",
	"240","241","242","243","244","245","246","247","248","249",
	"250","251","252","253","254","255"
};

inline void print_char_at_rgb(
	int x,
	int y,
	const unsigned char rgb[3],
	char c)
{
	static char buf_template[31] = "\033[000;000H\033[38;2;000;000;000m ";
	char buf[31]; // for cursor + RGB + char + \0
	memcpy(buf, buf_template, 31);

    // Overwrite the coordinates (y at 3..5, x at 7..9)
    memcpy(buf + 3, NUM_TO_CHAR_LUT[y], 3);
    memcpy(buf + 7, NUM_TO_CHAR_LUT[x], 3);

    // Overwrite RGB (r at 17..19, g at 21..23, b at 25..27)
    memcpy(buf + 17, NUM_TO_CHAR_LUT[rgb[0]], 3);
    memcpy(buf + 21, NUM_TO_CHAR_LUT[rgb[1]], 3);
    memcpy(buf + 25, NUM_TO_CHAR_LUT[rgb[2]], 3);

	// Character
	buf[29] = c;

	// Write all at once
	write(1, buf, 31);
}

////////////////////////////////////////////////////////////////////////////////

class Pipe
{
public:
	inline static unsigned idIncr = 0;
	unsigned id;
	int headX = 0;
	int headY = 0;
	uint8_t direction; // 0 north, 1 east, 2 south, 3 west
	uint8_t rgb[3] = {0};
	int *termWidth;
	int *termHeight;
	static constexpr uint8_t rotProba = 2; // out of 16

	int pipe_pos[116*2] = {0}; // Stores XY for each pipe already drawn.
	// 116 is the number of shades of the color (starting from 255) that i
	// will get if i fo x * 250 / 255 after each frame
	// so at frame 117 the first pixel will be black
	uint_fast8_t index = 0; // Index of the last pixel drawn

	Pipe(int *w, int *h) :
		id(idIncr++),
		direction(rand() & 0b11),
		termWidth(w), termHeight(h)
	{
		const uint8_t *val = HUE_LUT.data() + rand()%255 * 3;
		rgb[0] = *val, rgb[1] = *++val, rgb[2] = *++val;
	}

	void move()
	{
		// For each dir, + of - in the correct direction
		// then % to stay in the terminal
		switch (direction) {
		case 0: // North
			--headY %= *termHeight;
			break;
		case 2: // South
			++headY %= *termHeight;
			break;
		case 3: // West
			--headX %= *termWidth;
			break;
		case 1: // East
			++headX %= *termWidth;
			break;
		
		default:
			break;
		}

		print_char_at_rgb(headX, headY, rgb, '+');

		// Change direction
		unsigned char randN = rand() & 0xF;
		if (randN <= rotProba)
			++direction &= 0xF;
		else if (randN >= 10 - rotProba)
			--direction &= 0xF;
	}
};

class Pipes
{
private:
	std::vector<Pipe> pipes;
public:
	int terminalHeight = 0;
	int terminalWidth = 0;

	Pipes()
	{
		get_terminal_size(terminalWidth, terminalHeight);
	}
	// Next frame
	void next_frame()
	{}
};

////////////////////////////////////////////////////////////////////////////////

int main()
{
	srand(time(NULL));

	Pipes pipes;
	using clock = std::chrono::steady_clock;
    constexpr auto frame_time = std::chrono::milliseconds(50); // 20 FPS

	auto next = clock::now();

	while (true) {
		next += frame_time;
		pipes.next_frame();
		fflush(stdout);

		std::this_thread::sleep_until(next);
	}
	return 0;
}
