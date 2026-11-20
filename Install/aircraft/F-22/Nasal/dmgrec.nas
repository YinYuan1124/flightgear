# PHOENIX!
# SPYING in on damage.nas HEHEHE
# Used for really advanced Situational awareness
setprop("situ/lastlaunch","");
setprop("situ/engagedcs","");
var engagedBy = func(callsign,num1) {
    setprop("situ/engagedcs",callsign);
    screen.log.write("探测到交战，来自 "~callsign~"!",1,0.5,0);
}
var missileLaunch = func(cs) {
    #setprop("sim/multiplay/generic/string[8]","MSLWARN: Missile Launch Detected from :"~cs~": "~rand());
    # Check if hes on datalink
    var enemydlink = datalink.get_data(cs); # get our data
    if (enemydlink.on_link() == 1){
        # Thats not an enemy!!
        screen.log.write("探测到友方导弹发射",0,1,0);
    } else {
        screen.log.write("探测到敌方导弹发射",1,0.5,0);   
    }
}