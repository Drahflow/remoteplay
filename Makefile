all: pulse-sender pulse-receiver alsa-receiver

pulse-%: pulse-%.c common.h
	gcc -std=c11 -W -Wall -Wextra -pedantic -Werror -O4 -o $@ $< -lpulse

alsa-%: alsa-%.c common.h
	gcc -std=c11 -W -Wall -Wextra -pedantic -Werror -O4 -o $@ $< -lasound
