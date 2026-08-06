#!/bin/bash
export DISPLAY=:0
./OpenSwordigo/build/bin/swedit &
PID=$!
sleep 2
import -window root /home/quantumcreeper/.gemini/antigravity/brain/7e27218d-6612-4bb2-98c5-b2bc76241654/screenshot.jpg || scrot /home/quantumcreeper/.gemini/antigravity/brain/7e27218d-6612-4bb2-98c5-b2bc76241654/screenshot.jpg || maim /home/quantumcreeper/.gemini/antigravity/brain/7e27218d-6612-4bb2-98c5-b2bc76241654/screenshot.jpg
kill $PID
