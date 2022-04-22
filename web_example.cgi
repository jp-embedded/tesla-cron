#!/usr/bin/rrdcgi
<HTML>
<HEAD><TITLE>Tesla Cron</TITLE></HEAD>
<BODY>
<H1>Tesla Cron</H1>
<FORM>
<INPUT NAME=RRD_RES TYPE=RADIO VALUE=d>Day
<INPUT NAME=RRD_RES TYPE=RADIO VALUE=w>Week
<INPUT TYPE=SUBMIT>
</FORM> 

<H2>5YJ3E7EXXXXXXXXXX</H2>
<P>
<RRD::GRAPH "../rrd/tesla-5YJ3E7EXXXXXXXXXX-<RRD::CV RRD_RES>.svg" 
--imginfo '<IMG SRC=/rrd/%s WIDTH=%lu HEIGHT=%lu >'
-a SVG -w 600 -h 150
--start -1<RRD::CV RRD_RES> --end now
--vertical-label "EUR/Mwh Charge" 
DEF:price=/var/www/tmp/tesla-5YJ3EXXXXXXXXXX.rrd:price:AVERAGE
DEF:level=/var/www/tmp/tesla-5YJ3EXXXXXXXXXX.rrd:level:AVERAGE
DEF:window=/var/www/tmp/tesla-5YJ3EXXXXXXXXXX.rrd:window:AVERAGE
DEF:charging=/var/www/tmp/tesla-5YJ3EXXXXXXXXXX.rrd:charging:MAX
DEF:event=/var/www/tmp/tesla-5YJ3E7EXXXXXXXXXX.rrd:event:MAX
CDEF:window1=window,6,GE,120,0,IF AREA:window1#1F961F:"1h" 
CDEF:window2=window,5,GE,100,0,IF AREA:window2#3FA53F:"2h" 
CDEF:window3=window,4,GE,80,0,IF AREA:window3#5FB45F:"3h" 
CDEF:window4=window,3,GE,60,0,IF AREA:window4#7FC37F:"4h" 
CDEF:window5=window,2,GE,40,0,IF AREA:window5#9FD29F:"5h" 
CDEF:window6=window,1,GE,20,0,IF AREA:window6#BFE1BF:"6h Optimal Charge Window\n" 
CDEF:charging_scaled=charging,10,* AREA:charging_scaled#000000:"Charging" 
CDEF:event_scaled=event,120,* AREA:event_scaled#ff0000:"Calendar Event" 
LINE1:level#005500:"Battery level\n" 
LINE1:price#ff0000:"price" 
GPRINT:price:LAST:"cur\:%8.2lf %s" 
GPRINT:price:AVERAGE:"avg\:%8.2lf %s" 
GPRINT:price:MAX:"max\:%8.2lf %s\n" 
>
</P>


</BODY>
</HTML>

