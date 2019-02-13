/*
 File: JackBridge.h

MIT License

Copyright (c) 2016-2018 Shunji Uno <madhatter68@linux-dtm.ivory.ne.jp>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include <iostream>
#include <unistd.h>
#include <stdlib.h>
#include <string>
#include <sstream>
#include <cstdlib>
#include "jackClient.hpp"
#include "JackBridge.h"
#ifdef _WITH_MIDI_BRIDGE_
#include <rtmidi/RtMidi.h>
#define MAX_MIDI_PORTS 256
#endif // _WITH_MIDI_BRIDGE_

/*
 * JackBridge.cpp
 */
#define NUM_INPUT_CHANNELS  (NUM_INPUT_STREAMS*2)
#define NUM_OUTPUT_CHANNELS (NUM_OUTPUT_STREAMS*2)

class JackBridge : public JackClient, public JackBridgeDriverIF {
public:
    JackBridge(const char* name, int id, int num_Min, int num_Mout) : JackClient(name, JACK_PROCESS_CALLBACK), JackBridgeDriverIF(id) {
        if (attach_shm() < 0) {
            fprintf(stderr, "Attaching shared memory failed (id=%d)\n", id);
            exit(1);
        }

        isActive = false;
        isSyncMode = true; // FIXME: should be parameterized
        isVerbose = (getenv("JACKBRIDGE_DEBUG")) ? true : false;
        FrameNumber = 0;
        FramesPerBuffer = STRBUFNUM/2;
        *shmBufferSize = STRBUFSZ;
        *shmSyncMode = 0;

        config_audio_ports();
#ifdef _WITH_MIDI_BRIDGE_
        create_midi_ports(name, num_Min, num_Mout);
        register_ports((const char**)nameAin, (const char**)nameAout, (const char**)nameMin, (const char**)nameMout);
#else
        register_ports((const char**)nameAin, (const char**)nameAout, NULL, NULL);
#endif // _WITH_MIDI_BRIDGE_

        // For DEBUG
        lastHostTime = 0;
        struct mach_timebase_info theTimeBaseInfo;
        mach_timebase_info(&theTimeBaseInfo);
        double theHostClockFrequency = theTimeBaseInfo.denom / theTimeBaseInfo.numer;
        theHostClockFrequency *= 1000000000.0;
        HostTicksPerFrame = theHostClockFrequency / SampleRate;
        if (isVerbose) {
            printf("JackBridge#%d: Start with samplerate:%d Hz, buffersize:%d bytes\n",
                instance, SampleRate, BufSize);
        }
    }

    ~JackBridge() {
#ifdef _WITH_MIDI_BRIDGE_
        release_midi_ports();
#endif // _WITH_MIDI_BRIDGE_
    }

    int process_callback(jack_nframes_t nframes) override {
        sample_t *ain[NUM_INPUT_CHANNELS];
        sample_t *aout[NUM_OUTPUT_CHANNELS];

#ifdef _WITH_MIDI_BRIDGE_
        process_midi_message(nframes);
#endif // _WITH_MIDI_BRIDGE_

        if (*shmDriverStatus != JB_DRV_STATUS_STARTED) {
            // Driver isn't working. Just return zero buffer;
            for(int i=0; i<NUM_OUTPUT_CHANNELS; i++) {
                aout[i] = (sample_t*)jack_port_get_buffer(audioOut[i], nframes);
                bzero(aout[i], STRBUFSZ);
            }
            return 0;
        }

        // For DEBUG
        check_progress();

        if (!isActive) {
            ncalls = 0;
            FrameNumber = 0;

            if (isSyncMode) {
                *shmSyncMode = 1;
                *shmNumberTimeStamps = 0; 
                (*shmSeed)++;
            }

            isActive = true;
            printf("JackBridge#%d: Activated with SyncMode = %s, ZeroHostTime = %llx\n",
                instance, isSyncMode ? "Yes" : "No", *shmZeroHostTime);
        }

        if ((FrameNumber % FramesPerBuffer) == 0) {
            // FIXME: Should be atomic operation and do memory barrier
            if(*shmSyncMode == 1) {
                *shmZeroHostTime = mach_absolute_time();
                *shmNumberTimeStamps = FrameNumber / FramesPerBuffer;
                //(*shmNumberTimeStamps)++;
            } 

            if ((!isSyncMode) && isVerbose && ((ncalls++) % 100) == 0) {
                printf("JackBridge#%d: ZeroHostTime: %llx, %lld, diff:%d\n",
                    instance,  *shmZeroHostTime, *shmNumberTimeStamps,
                    ((int)(mach_absolute_time()+1000000-(*shmZeroHostTime)))-1000000);
            }
        }

        for(int i=0; i<NUM_INPUT_CHANNELS; i++) {
            ain[i] = (sample_t*)jack_port_get_buffer(audioIn[i], nframes);
        }
        sendToCoreAudio(ain, nframes);


        for(int i=0; i<NUM_OUTPUT_CHANNELS; i++) {
            aout[i] = (sample_t*)jack_port_get_buffer(audioOut[i], nframes);
        }
        receiveFromCoreAudio(aout, nframes);

        FrameNumber += nframes;

        return 0;
    }

    void setVerbose(bool flag) {
        printf("JackBridge#%d: Verbose mode %s.\n", instance, flag ? "on" : "off");
        isVerbose = flag;
    }

private:
    bool isActive, isSyncMode, isVerbose;
    bool showmsg;
    uint64_t lastHostTime;
    double HostTicksPerFrame;
    int64_t ncalls;
    char** nameAin;
    char** nameAout;

    int sendToCoreAudio(float** in,int nframes) {
        unsigned int offset = FrameNumber % FramesPerBuffer;
        // FIXME: should be consider buffer overwrapping
        for(int i=0; i<nframes; i++) {
            for(int j=0; j<NUM_INPUT_STREAMS; j++) {
                *(buf_down[j]+(offset+i)*2) = in[j*2][i];
                *(buf_down[j]+(offset+i)*2+1) = in[j*2+1][i];
            }
        }
        return nframes;
    }

    int receiveFromCoreAudio(float** out, int nframes) {
        //unsigned int offset = FrameNumber % FramesPerBuffer;
        unsigned int offset = (FrameNumber - nframes) % FramesPerBuffer;
        // FIXME: should be consider buffer overwrapping
        for(int i=0; i<nframes; i++) {
            for(int j=0; j<NUM_OUTPUT_STREAMS; j++) {
                out[j*2][i] = *(buf_up[j]+(offset+i)*2);
                out[j*2+1][i] = *(buf_up[j]+(offset+i)*2+1);
                *(buf_up[j]+(offset+i)*2) = 0.0f;
                *(buf_up[j]+(offset+i)*2+1) = 0.0f;
            }
        }
        return nframes;
    }

    void config_audio_ports() {
        nameAin = (char**)malloc(sizeof(char*)*(NUM_INPUT_CHANNELS+1));
        for(int i=0; i<NUM_INPUT_CHANNELS; i++) {
            nameAin[i] = (char*)malloc(256);
            snprintf(nameAin[i], 256, "input_%d", i+1);
        }
        nameAin[NUM_INPUT_CHANNELS] = nullptr;

        nameAout = (char**)malloc(sizeof(char*)*(NUM_OUTPUT_CHANNELS+1));
        for(int i=0; i<NUM_OUTPUT_CHANNELS; i++) {
            nameAout[i] = (char*)malloc(256);
            snprintf(nameAout[i], 256, "output_%d", i+1);
        }
        nameAout[NUM_OUTPUT_CHANNELS] = nullptr;
    }

#ifdef _WITH_MIDI_BRIDGE_
    RtMidiOut  **midiout;
    RtMidiIn   **midiin;
    int nOutPorts, nInPorts;
    char** nameMin;
    char** nameMout;

    int get_num_ports(unsigned long flags) {
        int num;
        const char** ports = jack_get_ports(client, "system", ".*raw midi", flags);
        if (!ports) {
            return 0;
        }

        for(num=0;*ports != NULL; ports++,num++) {
#if 0 // For DEBUG
            jack_port_t* p = jack_port_by_name(client, *ports);
            std::cout << ";" << *ports << ";" << jack_port_short_name(p) << ";" << jack_port_type(p) << std::endl;
#endif
        }
        return num;
    }

    void create_midi_ports(const char* name, int num_Min, int num_Mout) {
        char buf[256];

        // create bridge from Jack to CoreMIDI
        nOutPorts = (num_Mout < 0) ? get_num_ports(JackPortIsOutput) : num_Mout;
        midiout = (RtMidiOut**)malloc(sizeof(RtMidiOut*)*nOutPorts);
        nameMin = (char**)malloc(sizeof(char*)*(nOutPorts+1));

        for(int n=0; n<nOutPorts; n++) {
            try {
                midiout[n] = new RtMidiOut(RtMidi::MACOSX_CORE);
                snprintf(buf, 256, "%s %d", name, n+1);
                midiout[n]->openVirtualPort(buf);
            } catch ( RtMidiError &error ) {
                error.printMessage();
                exit( EXIT_FAILURE );
            }

            nameMin[n] = (char*)malloc(256);
            snprintf(nameMin[n], 256, "event_in_%d", n+1);
        }
        nameMin[nOutPorts] = NULL;

        // create bridge from CoreMIDI to Jack
        nInPorts = (num_Min < 0) ? get_num_ports(JackPortIsInput) : num_Min;
        midiin = (RtMidiIn**)malloc(sizeof(RtMidiIn*)*nInPorts);
        nameMout = (char**)malloc(sizeof(char*)*(nInPorts+1));

        for(int n=0; n<nInPorts; n++) {
            try {
                midiin[n] = new RtMidiIn(RtMidi::MACOSX_CORE);
                snprintf(buf, 256, "%s %d", name, n+1);
                midiin[n]->openVirtualPort(buf);
                midiin[n]->ignoreTypes(false, false, false);
            } catch ( RtMidiError &error ) {
                error.printMessage();
                exit( EXIT_FAILURE );
            }

            nameMout[n] = (char*)malloc(256);
            snprintf(nameMout[n], 256, "event_out_%d", n+1);
        }
        nameMout[nInPorts] = NULL;
    }

    void release_midi_ports() {
        // release bridge from Jack to CoreMIDI
        for(int n=0; n<nOutPorts; n++) {
            delete midiout[n];
            free(nameMin[n]);
        }
        free(midiout);
        free(nameMin);

        // release bridge from CoreMIDI to Jack
        for(int n=0; n<nInPorts; n++) {
            delete midiin[n];
            free(nameMout[n]);
        }
        free(midiin);
        free(nameMout);
    }

    void process_midi_message(jack_nframes_t nframes) {
        void *min, *mout;
        int count;
        jack_midi_event_t event;
        std::vector< unsigned char > message;
        jack_midi_data_t* buf;

        // process bridge from Jack to CoreMIDI
        for(int n=0; n<nOutPorts; n++) {
            min = jack_port_get_buffer(midiIn[n], nframes);
            count = jack_midi_get_event_count(min);
            for(int i=0; i<count; i++) {
                jack_midi_event_get(&event, min, i);
                message.clear();
                for (int j=0; j<event.size; j++) {
                    message.push_back(event.buffer[j]);
                }
                if (message.size() > 0) {
                    midiout[n]->sendMessage(&message);
                }
            }
        }

        // process bridge from CoreMIDI to Jack
        for(int n=0; n<nInPorts; n++) {
            mout = jack_port_get_buffer(midiOut[n], nframes);
            jack_midi_clear_buffer(mout);
            midiin[n]->getMessage(&message);
            while(message.size() > 0) {
                buf = jack_midi_event_reserve(mout, 0, message.size());
                if (buf != NULL) {
                    for(int i=0; i<message.size(); i++) {
                        buf[i] = message[i];
                    }
                } else {
                    fprintf(stderr, "ERROR: jack_midi_event_reserve failed()\n");
                }
                midiin[n]->getMessage(&message);
            }
        }
    }
#endif // _WITH_MIDI_BRIDGE_

    void check_progress() {
#if 0
        if (isVerbose && ((ncalls++) % 500) == 0) {
            printf("JackBridge#%d: FRAME %llu : Write0: %llu Read0: %llu Write1: %llu Read0: %llu\n",
                 instance, FrameNumber,
                 *shmWriteFrameNumber[0], *shmReadFrameNumber[0],
                 *shmWriteFrameNumber[1], *shmReadFrameNumber[1]);
        }
#endif

        int diff = *shmWriteFrameNumber[0] - FrameNumber;
        int interval = (mach_absolute_time() - lastHostTime) / HostTicksPerFrame;
        if (showmsg) {
            if ((diff >= (STRBUFNUM/2))||(interval >= BufSize*2))  {
                if (isVerbose) {
                    printf("WARNING: miss synchronization detected at FRAME %llu (diff=%d, interval=%d)\n",
                        FrameNumber, diff, interval);
                    fflush(stdout);
                }
                showmsg = false;
            }
        } else {
            if (diff < (STRBUFNUM/2)) {
                showmsg = true;
            }
        }
        lastHostTime = mach_absolute_time();
    }
};

int
main(int argc, char** argv)
{
    JackBridge* jackBridge[NUM_INSTANCES];
    int ch, num_midiIn=-1, num_midiOut=-1;
    bool vflag=false;

    while ((ch = getopt(argc, argv, "vi:o:")) != -1) {
        switch (ch) {
            case 'v':
                vflag = true;
                break;
#ifdef _WITH_MIDI_BRIDGE_
            case 'i':
                num_midiIn = atoi(optarg);
                if (num_midiIn > MAX_MIDI_PORTS) {
                    fprintf(stderr, "%s: exceed maximum MIDI Inputs number (> %d)\n", argv[0], MAX_MIDI_PORTS);
                }
                break;

            case 'o':
                num_midiOut = atoi(optarg);
                if (num_midiOut > MAX_MIDI_PORTS) {
                    fprintf(stderr, "%s: exceed maximum MIDI Outputs number (> %d)\n", argv[0], MAX_MIDI_PORTS);
                }
                break;
#endif
             default:
                fprintf(stderr, "Usage: %s [-v] [-i <# of MIDI-In>] [-o <# of MIDI-Out>]\n", argv[0]);
                return -1;
        }
    }

    // Create instances of jack client
    jackBridge[0] = new JackBridge("JackBridge #1", 0, num_midiIn, num_midiOut);
    if (vflag) {
        jackBridge[0]->setVerbose(vflag);
    }
    //jackBridge[1] = new JackBridge("JackBridge #2", 1);

    // activate gateway from/to jack ports
    jackBridge[0]->activate();
    //jackBridge[1]->activate();

    // Infinite loop until daemon is killed.
    while(1) {
        sleep(600);
    }

    return 0;
}
