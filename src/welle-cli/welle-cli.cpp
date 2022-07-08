#include <sys/stat.h>
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <set>
#include <utility>
#include <cstdio>
#include <unistd.h>
#if defined(HAVE_ALSA)
#  include "welle-cli/alsa-output.h"
#endif
#include "backend/radio-receiver.h"
#include "input/input_factory.h"
#include "various/channels.h"
#include "libs/json.hpp"
extern "C" {
#include "various/wavfile.h"
}

using namespace std;
using namespace nlohmann;

const std::string WHITESPACE = " \n\r\t\f\v";
 
std::string ltrim(const std::string &s)
{
    size_t start = s.find_first_not_of(WHITESPACE);
    return (start == std::string::npos) ? "" : s.substr(start);
}

std::string rtrim(const std::string &s)
{
    size_t end = s.find_last_not_of(WHITESPACE);
    return (end == std::string::npos) ? "" : s.substr(0, end + 1);
}
 
std::string trim(const std::string &s) {
    return rtrim(ltrim(s));
}

class WavProgrammeHandler: public ProgrammeHandlerInterface {
    public:
        WavProgrammeHandler(uint32_t SId, const std::string& fileprefix) :
            SId(SId),
            filePrefix(fileprefix) {}
        ~WavProgrammeHandler() {
            if (fd) {
                wavfile_close(fd);
            }
        }
        WavProgrammeHandler(const WavProgrammeHandler& other) = delete;
        WavProgrammeHandler& operator=(const WavProgrammeHandler& other) = delete;
        WavProgrammeHandler(WavProgrammeHandler&& other) = default;
        WavProgrammeHandler& operator=(WavProgrammeHandler&& other) = default;

        virtual void onFrameErrors(int frameErrors) override { (void)frameErrors; }
        virtual void onNewAudio(std::vector<int16_t>&& audioData, int sampleRate, const string& mode) override
        {
            if (rate != sampleRate) {
                cout << "[0x" << std::hex << SId << std::dec << "] " <<
                    "rate " << sampleRate <<  " mode " << mode << endl;

                string filename = filePrefix + ".wav";
                if (fd) {
                    wavfile_close(fd);
                }
                fd = wavfile_open(filename.c_str(), sampleRate, 2);

                if (not fd) {
                    cerr << "Could not open wav file " << filename << endl;
                }
            }
            rate = sampleRate;

            if (fd) {
                wavfile_write(fd, audioData.data(), audioData.size());
            }
        }

        virtual void onRsErrors(bool uncorrectedErrors, int numCorrectedErrors) override {
            (void)uncorrectedErrors; (void)numCorrectedErrors; }
        virtual void onAacErrors(int aacErrors) override { (void)aacErrors; }
        virtual void onNewDynamicLabel(const std::string& label) override
        {
            cout << "[0x" << std::hex << SId << std::dec << "] " << "DLS: " << label << endl;

            ofstream myfile;
            string filename_dls = filePrefix + ".txt";
            unsigned long int timestamp = time(NULL);

            myfile.open(filename_dls, std::ios_base::app);

            json j;
            j["dls"] = {
                {"value", trim(label)},
                {"ts", timestamp}
            };
            myfile << j << endl;

            //myfile << "DYNAMIC_LABEL=" << label << endl;
            myfile.close();
        }

        virtual void onMOT(const mot_file_t& mot_file) override
        {
            string extension;
            if (mot_file.content_sub_type == 0x01) {
                extension = "jpg";
            } else if (mot_file.content_sub_type == 0x03) {
                extension = "png";
            }

            ofstream file_mot, file_txt;
            unsigned long int timestamp = time(NULL);

            uint32_t current_mot_size = mot_file.data.size();
            if (current_mot_size == last_size) {
                cout << "[0x" << std::hex << SId << std::dec << "] MOT BYPASS (doublon " << last_size << " octets)" << endl;
                return;
            }
            last_size = current_mot_size;

            string filename_mot = filePrefix + "-" + std::to_string(timestamp) + "." + extension;
            string filename_txt = filePrefix + ".txt";
            file_txt.open(filename_txt, std::ios_base::app);

            json j;
            j["mot"] = {
                {"file", filename_mot.substr(filename_mot.find_last_of("/\\") + 1)},
                {"content_name", mot_file.content_name},
                {"click_through_url", mot_file.click_through_url},
                {"category_title", mot_file.category_title},
                {"ts", timestamp}
            };

            file_txt << j << endl;
            file_txt.close();

            file_mot.open(filename_mot);
            std::stringstream ss;
            for (auto it = mot_file.data.begin(); it != mot_file.data.end(); it++)    {
                ss << *it;
            }
            file_mot << ss.str();
            file_mot.close();

            cout << "[0x" << std::hex << SId << std::dec << "] MOT reçu " << endl;
        }

        virtual void onPADLengthError(size_t announced_xpad_len, size_t xpad_len) override
        {
            cout << "X-PAD length mismatch, expected: " << announced_xpad_len << " got: " << xpad_len << endl;
        }

    private:
        uint32_t last_size = 0;
        uint32_t SId;
        string filePrefix;
        FILE* fd = nullptr;
        int rate = 0;
};


class RadioInterface : public RadioControllerInterface {
    public:
        // rapport signal sur bruit
        virtual void onSNR(float snr) override {
            unsigned long int timestamp = time(NULL);
            json j;

            j["snr"] = {
                {"ts", timestamp},
                {"value", snr}
            };

            if (last_snr != j) {
                cout << j << endl;
                last_snr = j;
            }

        }
        virtual void onFrequencyCorrectorChange(int /*fine*/, int /*coarse*/) override { }
        virtual void onSyncChange(char isSync) override { synced = isSync; }
        virtual void onSignalPresence(bool /*isSignal*/) override { }
        virtual void onServiceDetected(uint32_t sId) override
        {
            cout << "New Service: 0x" << hex << sId << dec << endl;
            serviceId = sId;
        }

        virtual void onNewEnsemble(uint16_t eId) override
        {
            //cout << "Ensemble name id: " << hex << eId << dec << endl;
            ensembleId = eId;
        }

        virtual void onSetEnsembleLabel(DabLabel& label) override
        {
            //cout << "Ensemble label: " << label.utf8_label() << endl;
            ensembleLabel = label.utf8_label();
        }

        virtual void onDateTimeUpdate(const dab_date_time_t& dateTime) override
        {
            json j;
            j["UTCTime"] = {
                {"year", dateTime.year},
                {"month", dateTime.month},
                {"day", dateTime.day},
                {"hour", dateTime.hour},
                {"minutes", dateTime.minutes},
                {"seconds", dateTime.seconds}
            };

            if (last_date_time != j) {
                //cout << j << endl;
                last_date_time = j;
            }
        }

        virtual void onFIBDecodeSuccess(bool crcCheckOk, const uint8_t* fib) override { }
        virtual void onNewImpulseResponse(std::vector<float>&& data) override { (void)data; }
        virtual void onNewNullSymbol(std::vector<DSPCOMPLEX>&& data) override { (void)data; }
        virtual void onConstellationPoints(std::vector<DSPCOMPLEX>&& data) override { (void)data; }
        virtual void onMessage(message_level_t level, const std::string& text, const std::string& text2 = std::string()) override
        {
            std::string fullText;
            if (text2.empty())
                fullText = text;
            else
                fullText = text + text2;

            switch (level) {
                case message_level_t::Information:
                    cerr << "Info: " << fullText << endl;
                    break;
                case message_level_t::Error:
                    cerr << "Error: " << fullText << endl;
                    break;
            }
        }

        virtual void onTIIMeasurement(tii_measurement_t&& m) override
        {
            json j;
            j["TII"] = {
                {"comb", m.comb},
                {"pattern", m.pattern},
                {"delay", m.delay_samples},
                {"delay_km", m.getDelayKm()},
                {"error", m.error}
            };
            cout << j << endl;
        }

        json last_snr;
        json last_date_time;
        bool synced = false;
        FILE* fic_fd = nullptr;

        int serviceId = 0;
        string serviceLabel = "";
        int ensembleId = 0;
        string ensembleLabel = "";
};

struct options_t {
    string soapySDRDriverArgs = "";
    string antenna = "";
    int gain = -1;
    string channel = "10B";
    string iqsource = "";
    string programme = "GRRIF";
    string frontend = "auto";
    string frontend_args = "";
    string dump_directory = "";
    bool dump_programme = true;
    bool decode_all_programmes = true;
    bool fic_rec = false;
    list<int> tests;

    RadioReceiverOptions rro;
};

options_t parse_cmdline(int argc, char **argv)
{
    options_t options;
    options.rro.decodeTII = false;

    int opt;
    while ((opt = getopt(argc, argv, "c:o:g:p:u")) != -1) {
        switch (opt) {
            case 'c':
                options.channel = optarg;
                break;
            case 'o':
                options.dump_directory = optarg;
                break;
            case 'g':
                options.gain = std::atoi(optarg);
                break;
            case 'u':
                options.rro.disableCoarseCorrector = true;
                break;
            default:
                cerr << "Unknown option." << endl;
                exit(1);
        }
    }

    return options;
}

int main(int argc, char **argv)
{
    auto options = parse_cmdline(argc, argv);

    RadioInterface ri;

    Channels channels;

    unique_ptr<CVirtualInput> in = nullptr;

    if (options.iqsource.empty()) {
        in.reset(CInputFactory::GetDevice(ri, options.frontend));

        if (not in) {
            cerr << "Could not start device" << endl;
            return 1;
        }
    }

    if (options.gain == -1) {
        in->setAgc(true);
    }
    else {
        in->setGain(options.gain);
    }

    auto freq = channels.getFrequency(options.channel);
    in->setFrequency(freq);
    string service_to_tune = options.programme;

    RadioReceiver rx(ri, *in, options.rro);

    rx.restart(false);

    cerr << "Wait for sync" << endl;
    while (not ri.synced) {
        this_thread::sleep_for(chrono::seconds(3));
    }

    cerr << "Wait for service list" << endl;
    while (rx.getServiceList().empty()) {
        this_thread::sleep_for(chrono::seconds(1));
    }

    // Wait an additional 3 seconds so that the receiver can complete the service list
    this_thread::sleep_for(chrono::seconds(3));

    if (options.decode_all_programmes) {
        using SId_t = uint32_t;
        map<SId_t, WavProgrammeHandler> phs;

        cerr << "Service list" << endl;
        for (const auto& s : rx.getServiceList()) {
            cerr << "  [0x" << std::hex << s.serviceId << std::dec << "] " << s.serviceLabel.utf8_label() << " ";
            for (const auto& sc : rx.getComponents(s)) {
                cerr << " [component "  << sc.componentNr <<
                    " ASCTy: " <<
                    (sc.audioType() == AudioServiceComponentType::DABPlus ? "DAB+" : "unknown") << " ]";

                const auto& sub = rx.getSubchannel(sc);
                cerr << " [subch " << sub.subChId << " bitrate:" << sub.bitrate() << " at SAd:" << sub.startAddr << "]";
            }
            cerr << endl;

            std::stringstream stream;
            stream << "0x" << std::setfill('0') << std::hex << s.serviceId;
            string dumpFilePrefix = options.dump_directory + "/" + stream.str();

            mkdir(dumpFilePrefix.c_str(), 0755);

            dumpFilePrefix += "/" + stream.str();

            dumpFilePrefix.erase(std::find_if(dumpFilePrefix.rbegin(), dumpFilePrefix.rend(),
                        [](int ch) { return !std::isspace(ch); }).base(), dumpFilePrefix.end());

            ofstream myfile;
            string filename_sid = dumpFilePrefix + ".txt";
            myfile.open(filename_sid, std::ios_base::app);
            unsigned long int timestamp = time(NULL);

            json je;
            je["ensemble"] = {
                {"emsembleId", ri.ensembleId},
                {"ensembleLabel", trim(ri.ensembleLabel)},
                {"ts", timestamp}
            };
            myfile << je << endl;

            json js;
            js["service"] = {
                {"serviceId", s.serviceId},
                {"serviceLabel", trim(s.serviceLabel.utf8_label())},
                {"ts", timestamp}
            };
            myfile << js << endl;

            myfile.close();

            WavProgrammeHandler ph(s.serviceId, dumpFilePrefix);
            phs.emplace(std::make_pair(s.serviceId, move(ph)));

            auto dumpFileName = dumpFilePrefix + ".msc";

            if (rx.addServiceToDecode(phs.at(s.serviceId), dumpFileName, s) == false) {
                cerr << "Tune to " << service_to_tune << " failed" << endl;
            }
        }

        while (true) {
            cerr << "**** Enter '.' to quit." << endl;
            cin >> service_to_tune;
            if (service_to_tune == ".") {
                break;
            }
        }
    }
    else {
        cerr << "Nothing to do, not ALSA support." << endl;
    }

    return 0;
}