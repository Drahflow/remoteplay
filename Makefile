all: pulse-sender pulse-receiver alsa-receiver pulse-calibration

pulse-calibration: pulse-calibration.c
	gcc -std=c11 -W -Wall -Wextra -pedantic -Werror -O4 -o $@ $< -lpulse -lm

pulse-%: pulse-%.c common.h
	gcc -std=c11 -W -Wall -Wextra -pedantic -Werror -O4 -o $@ $< -lpulse

alsa-%: alsa-%.c common.h
	gcc -std=c11 -W -Wall -Wextra -pedantic -Werror -O4 -o $@ $< -lasound
