kwahzolin v0.2.6

a benjolin-inspired synthesizer module for the ableton move unofficial firmware [schwung](https://github.com/charlesvestal/schwung). osc2 clocks the register on every positive zero crossing. osc1's sign becomes the new bit. the accumulated register value spills back as voltage. this is a rungler which is rob hordijk's circuit. rest in peace rob. 

2 oscillators, triangle-shaped, circling each other. osc2 is the clock. every upswing pushes a new bit into the 8-bit shift register from osc1's sign. the register value feeds back into both oscillator frequencies and into the filter's cutoff. three independent lfos. each can  target any of the 8 knob parameters at audio-rate resolution.

knob 1 - osc 1 frequency. base pitch of the first triangle oscillator. the rungler and lfo can sweep it. 
knob 2 - osc 2 frequency. clock rate of the kwahzolin. slow osc2 means the rungler evolves languidly. fast osc2 and everything seizes up.
knob 3 - osc chaos. how deep the rungler effects both oscillator frequencies. 
knob 4 - filter cutoff. thee resting mouth of the state variable filter. the rungler and lfo drag it around. 
knob 5 - filter resonance. filter self-feedback amount. 
knob 6 - filter chaos. scales how far the rungler throws the cutoff on each clock tick. 
knob 7 - cross modulation. bidirectional frequency modulation between the two oscillators. each oscillator's frequency is offset by the other's triangle output. 
knob 8 - loop. turing machine register control.