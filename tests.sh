#!/bin/sh
WAV=${1:-/tmp/you_wont_be_satisfied.wav}
RM=""

INPORTS="system:capture_1 system:capture_2"
OUTPORTS="system:playback_1 system:playback_2"

if [ ! -f $WAV ]; then
  echo "# wav file missing - generating $WAV"
  sox -n $WAV synth 3 sine 300-3300 gain -5
	RM=true
fi

if true; then
  ./jack-stdout -d 2 -e signed   -b  8    $INPORTS   | ./jack-stdin -e signed   -b  8    $OUTPORTS
  ./jack-stdout -d 2 -e signed   -b  8 -B $INPORTS   | ./jack-stdin -e signed   -b  8 -B $OUTPORTS
  ./jack-stdout -d 2 -e unsigned -b  8    $INPORTS   | ./jack-stdin -e unsigned -b  8    $OUTPORTS
  ./jack-stdout -d 2 -e unsigned -b  8 -B $INPORTS   | ./jack-stdin -e unsigned -b  8 -B $OUTPORTS

  ./jack-stdout -d 2 -e signed   -b 16    $INPORTS   | ./jack-stdin -e signed   -b 16    $OUTPORTS
  ./jack-stdout -d 2 -e signed   -b 16 -B $INPORTS   | ./jack-stdin -e signed   -b 16 -B $OUTPORTS
  ./jack-stdout -d 2 -e unsigned -b 16    $INPORTS   | ./jack-stdin -e unsigned -b 16    $OUTPORTS
  ./jack-stdout -d 2 -e unsigned -b 16 -B $INPORTS   | ./jack-stdin -e unsigned -b 16 -B $OUTPORTS

  ./jack-stdout -d 2 -e signed   -b 24    $INPORTS   | ./jack-stdin -e signed   -b 24    $OUTPORTS
  ./jack-stdout -d 2 -e signed   -b 24 -B $INPORTS   | ./jack-stdin -e signed   -b 24 -B $OUTPORTS
  ./jack-stdout -d 2 -e unsigned -b 24    $INPORTS   | ./jack-stdin -e unsigned -b 24    $OUTPORTS
  ./jack-stdout -d 2 -e unsigned -b 24 -B $INPORTS   | ./jack-stdin -e unsigned -b 24 -B $OUTPORTS

  ./jack-stdout -d 2 -e signed   -b 32    $INPORTS   | ./jack-stdin -e signed   -b 32    $OUTPORTS
  ./jack-stdout -d 2 -e signed   -b 32 -B $INPORTS   | ./jack-stdin -e signed   -b 32 -B $OUTPORTS
  ./jack-stdout -d 2 -e unsigned -b 32    $INPORTS   | ./jack-stdin -e unsigned -b 32    $OUTPORTS
  ./jack-stdout -d 2 -e unsigned -b 32 -B $INPORTS   | ./jack-stdin -e unsigned -b 32 -B $OUTPORTS

  ./jack-stdout -d 2 -e float    -b 32    $INPORTS   | ./jack-stdin -e float    -b 32    $OUTPORTS
  ./jack-stdout -d 2 -e float    -b 32 -B $INPORTS   | ./jack-stdin -e float    -b 32 -B $OUTPORTS
fi

if true; then
  sox $WAV -t raw -r 48k -e signed   -b  8 -c 2    - | ./jack-stdin -e signed   -b 8     $OUTPORTS
  sox $WAV -t raw -r 48k -e signed   -b  8 -c 2 -B - | ./jack-stdin -e signed   -b 8 -B  $OUTPORTS
  sox $WAV -t raw -r 48k -e unsigned -b  8 -c 2    - | ./jack-stdin -e unsigned -b 8     $OUTPORTS
  sox $WAV -t raw -r 48k -e unsigned -b  8 -c 2 -B - | ./jack-stdin -e unsigned -b 8 -B  $OUTPORTS

  sox $WAV -t raw -r 48k -e signed   -b 16 -c 2    - | ./jack-stdin                      $OUTPORTS
  sox $WAV -t raw -r 48k -e signed   -b 16 -c 2 -B - | ./jack-stdin                   -B $OUTPORTS
  sox $WAV -t raw -r 48k -e unsigned -b 16 -c 2    - | ./jack-stdin -e unsigned -b 16    $OUTPORTS
  sox $WAV -t raw -r 48k -e unsigned -b 16 -c 2 -B - | ./jack-stdin -e unsigned -b 16 -B $OUTPORTS

  sox $WAV -t raw -r 48k -e signed   -b 24 -c 2    - | ./jack-stdin -e signed   -b 24    $OUTPORTS
  sox $WAV -t raw -r 48k -e signed   -b 24 -c 2 -B - | ./jack-stdin -e signed   -b 24 -B $OUTPORTS
  sox $WAV -t raw -r 48k -e unsigned -b 24 -c 2    - | ./jack-stdin -e unsigned -b 24    $OUTPORTS
  sox $WAV -t raw -r 48k -e unsigned -b 24 -c 2 -B - | ./jack-stdin -e unsigned -b 24 -B $OUTPORTS

  sox $WAV -t raw -r 48k -e signed   -b 32 -c 2    - | ./jack-stdin -e signed   -b 32    $OUTPORTS
  sox $WAV -t raw -r 48k -e signed   -b 32 -c 2 -B - | ./jack-stdin -e signed   -b 32 -B $OUTPORTS
  sox $WAV -t raw -r 48k -e unsigned -b 32 -c 2    - | ./jack-stdin -e unsigned -b 32    $OUTPORTS
  sox $WAV -t raw -r 48k -e unsigned -b 32 -c 2 -B - | ./jack-stdin -e unsigned -b 32 -B $OUTPORTS

  sox $WAV -t raw -r 48k -e float    -b 32 -c 2    - | ./jack-stdin -e float    -b 32    $OUTPORTS
  sox $WAV -t raw -r 48k -e float    -b 32 -c 2 -B - | ./jack-stdin -e float    -b 32 -B $OUTPORTS
fi

test -n "$RM" && rm $WAV
