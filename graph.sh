#!/bin/sh

for f in /var/tmp/tesla-*.rrd
do
   for t in d w
   do
      out="${f%.rrd}-$t.svg"
      echo "$f -> $out"

      rrdtool graph $out -a SVG -w 600 -h 150 --start -1$t --end now --vertical-label "EUR/Mwh Charge" \
--imginfo '<IMG SRC=%s WIDTH=%lu HEIGHT=%lu >' \
DEF:price=$f:price:AVERAGE \
DEF:level=$f:level:AVERAGE \
DEF:window=$f:window:AVERAGE \
DEF:charging=$f:charging:MAX \
DEF:event=$f:event:MAX \
CDEF:window1=window,6,GE,120,0,IF AREA:window1#1F961F:"1h" \
CDEF:window2=window,5,GE,100,0,IF AREA:window2#3FA53F:"2h" \
CDEF:window3=window,4,GE,80,0,IF AREA:window3#5FB45F:"3h" \
CDEF:window4=window,3,GE,60,0,IF AREA:window4#7FC37F:"4h" \
CDEF:window5=window,2,GE,40,0,IF AREA:window5#9FD29F:"5h" \
CDEF:window6=window,1,GE,20,0,IF AREA:window6#BFE1BF:"6h Optimal Charge Window\n" \
CDEF:charging_scaled=charging,10,* AREA:charging_scaled#000000:"Charging" \
CDEF:event_scaled=event,120,* AREA:event_scaled#ff0000:"Calendar Event" \
LINE1:level#005500:"Battery level\n" \
LINE1:price#ff0000:"price" \
GPRINT:price:LAST:"cur\:%8.2lf %s" \
GPRINT:price:AVERAGE:"avg\:%8.2lf %s" \
GPRINT:price:MAX:"max\:%8.2lf %s\n" 

   done
done

