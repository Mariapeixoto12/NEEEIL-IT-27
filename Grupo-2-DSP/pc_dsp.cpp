#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using Complex = std::complex<float>;
using Spectrum = std::vector<Complex>;
using Bins = std::vector<float>;

const float PI = 3.14159265f;

const int frameSize = 512;
const int hopSize = frameSize / 2;
const float oversubtraction = 4.0f;
const float minimumGain = 0.05f;
const float powerSmoothing = 0.6f;
const float gainSmoothing = 0.4f;
const float noiseWindowSeconds = 1.5f;


Spectrum fft(const Spectrum& samples)
{
    int n = samples.size(); // number of samples per frame
    if (n == 1) return samples; // caso base

    Spectrum evens, odds;
    for (int i = 0; i < n; i += 2) {    // este loop apenas separa os samples nos dois grupos (pares e ímpares)
        evens.push_back(samples[i]);
        odds.push_back(samples[i + 1]);
    }
    Spectrum evenSpectrum = fft(evens); // recursion yay
    Spectrum oddSpectrum = fft(odds);   // aw shit, here we go again

    Spectrum result(n);
    for (int k = 0; k < n / 2; k++) {   // what's going on here?
        Complex turn = std::polar(1.0f, -2 * PI * k / n) * oddSpectrum[k];
        result[k] = evenSpectrum[k] + turn;
        result[k + n / 2] = evenSpectrum[k] - turn;
    }
    return result;
}

Spectrum inverse_fft(Spectrum spectrum)
{
    for (Complex& value : spectrum) value = std::conj(value);
    Spectrum result = fft(spectrum);    // fft of what already is a spectrum in the frequency domain?
    for (Complex& value : result) value = std::conj(value) / float(result.size());
    return result;
}


Bins sqrt_hann_window()
{
    Bins window(frameSize);
    for (int i = 0; i < frameSize; i++) {
        float hann = 0.5f - 0.5f * std::cos(2 * PI * i / frameSize);
        window[i] = std::sqrt(hann);
    }
    return window;
}

Spectrum spectrum_of_frame(const Bins& audio, int start, const Bins& window)
{
    Spectrum frame(frameSize);
    for (int i = 0; i < frameSize; i++) {
        frame[i] = audio[start + i] * window[i];    // multiply the frame by the window
    }
    return fft(frame);
}

Bins power_of(const Spectrum& spectrum)
{
    Bins power(spectrum.size());
    for (size_t bin = 0; bin < spectrum.size(); bin++) {
        power[bin] = std::norm(spectrum[bin]);  // std::norm already returns magnitude² for some reason
    }
    return power;
}

Bins blend(const Bins& old, const Bins& fresh, float howMuchToKeepOld)
{
    Bins result(old.size());
    for (size_t bin = 0; bin < old.size(); bin++) {
        result[bin] = howMuchToKeepOld * old[bin] + (1 - howMuchToKeepOld) * fresh[bin];
    }
    return result;
}

Bins lowest_in_each_bin(const std::vector<Bins>& recentFrames)
{
    Bins lowest = recentFrames[0];
    for (const Bins& frame : recentFrames) {
        for (size_t bin = 0; bin < frame.size(); bin++) {
            lowest[bin] = std::min(lowest[bin], frame[bin]);
        }
    }
    return lowest;
}

// applies the G[k] = max(1 - sqrt(alpha * P_n / P_s), beta)
Bins gain_from_noise(const Bins& signalPower, const Bins& noisePower)
{
    Bins gain(signalPower.size());
    for (size_t bin = 0; bin < gain.size(); bin++) {
        float noiseToSignal = oversubtraction * noisePower[bin] / (signalPower[bin] + 1e-12f);
        gain[bin] = std::max(1 - std::sqrt(noiseToSignal), minimumGain);
    }
    return gain;
}

// why is this needed?
Bins smooth_across_neighbours(const Bins& gain)
{
    size_t n = gain.size();
    Bins smoothed(n);
    for (size_t bin = 0; bin < n; bin++) {
        size_t binBelow = (bin + n - 1) % n;
        size_t binAbove = (bin + 1) % n;
        smoothed[bin] = (gain[binBelow] + gain[bin] + gain[binAbove]) / 3;
    }
    return smoothed;
}


Bins denoise(const Bins& noisy, int sampleRate)
{
    Bins window = sqrt_hann_window();
    int frameCount = (noisy.size() - frameSize) / hopSize + 1;
    int framesInNoiseWindow = std::round(noiseWindowSeconds * sampleRate / hopSize);

    Bins output(noisy.size(), 0.0f);
    Bins smoothedPower, previousGain;
    std::vector<Bins> recentPower;  // this is a vector of the frames of smoothed power in the 1.5s noise window

    for (int frame = 0; frame < frameCount; frame++) {  // for each frame
        int start = frame * hopSize;

        Spectrum spectrum = spectrum_of_frame(noisy, start, window);
        Bins power = power_of(spectrum);

        if (frame == 0) {   // if considering the first frame
            smoothedPower = power;
            recentPower.assign(framesInNoiseWindow, power);
        }
        smoothedPower = blend(smoothedPower, power, powerSmoothing);    // smooth the power between frames
        recentPower[frame % framesInNoiseWindow] = smoothedPower;   // recentPower basically works as a FIFO that starts replacing itself at n = framesInNoiseWindow
        Bins noisePower = lowest_in_each_bin(recentPower);  // noise estimation

        Bins gain = smooth_across_neighbours(gain_from_noise(smoothedPower, noisePower));
        if (frame == 0) previousGain = gain;
        gain = blend(previousGain, gain, gainSmoothing);    // more smoothing the gains?
        previousGain = gain;

        for (int bin = 0; bin < frameSize; bin++) {
            spectrum[bin] *= gain[bin]; // if the noise is 0 isn't this basically jsut quaring the spectrum's gains? shouldn't it just be spectrum[bin] = gain[bin]?
        }
        Spectrum cleanedFrame = inverse_fft(spectrum);  // the inverse_fft still gives a spectrum? shouldn't it do frequency -> time?

        for (int i = 0; i < frameSize; i++) {   // for each sample in the current frame 'frame'
            output[start + i] += cleanedFrame[i].real() * window[i];    // the sum of the contributions at (n/2)+k and at the next frame's k added together is always 1
        }
    }
    return output;
}


uint32_t read_number(std::istream& file, int byteCount)
{
    uint32_t value = 0;
    for (int i = 0; i < byteCount; i++) {
        value |= uint32_t(file.get() & 0xFF) << (8 * i);
    }
    return value;
}

void write_number(std::ostream& file, uint32_t value, int byteCount)
{
    for (int i = 0; i < byteCount; i++) {
        file.put(char((value >> (8 * i)) & 0xFF));
    }
}

bool read_wav(const std::string& path, Bins& samples, int& sampleRate)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;

    const int riffHeaderSize = 12;
    file.ignore(riffHeaderSize);

    int channels = 1;
    while (file) {
        char name[5] = {};
        file.read(name, 4);
        uint32_t size = read_number(file, 4);
        if (!file) break;

        if (std::string(name) == "fmt ") {
            int format = read_number(file, 2);
            channels = read_number(file, 2);
            sampleRate = read_number(file, 4);
            file.ignore(6);
            int bitsPerSample = read_number(file, 2);
            file.ignore(size - 16 + (size & 1));

            if (format != 1 || bitsPerSample != 16) {
                std::cerr << "Need 16-bit PCM. Convert with:\n"
                          << "  ffmpeg -i in.wav -ac 1 -ar 16000 -c:a pcm_s16le input.wav\n";
                return false;
            }
        } else if (std::string(name) == "data") {
            uint32_t frameCount = size / (2 * channels);    // 1 sample (16 bits ~ 2 bytes) of each channel per frame
            for (uint32_t i = 0; i < frameCount; i++) {
                int16_t firstChannel = int16_t(read_number(file, 2));
                file.ignore(2 * (channels - 1));
                if (!file) break;
                samples.push_back(firstChannel / 32768.0f); // turns the two bites into floats from -1.0 to just under 1.0
            }
            return true;
        } else {
            file.ignore(size + (size & 1));
        }
    }
    return false;
}

void write_wav(const std::string& path, const Bins& samples, int sampleRate)
{
    std::ofstream file(path, std::ios::binary);
    uint32_t dataSize = samples.size() * 2;

    file << "RIFF";
    write_number(file, 36 + dataSize, 4);
    file << "WAVEfmt ";
    write_number(file, 16, 4);
    write_number(file, 1, 2);
    write_number(file, 1, 2);
    write_number(file, sampleRate, 4);
    write_number(file, sampleRate * 2, 4);
    write_number(file, 2, 2);
    write_number(file, 16, 2);
    file << "data";
    write_number(file, dataSize, 4);

    for (float sample : samples) {
        float scaled = std::clamp(sample * 32768.0f, -32768.0f, 32767.0f);
        write_number(file, uint16_t(int16_t(std::lround(scaled))), 2);
    }
}

float root_mean_square(const Bins& samples)
{
    double sumOfSquares = 0;
    for (float sample : samples) sumOfSquares += double(sample) * sample;
    return std::sqrt(sumOfSquares / samples.size());
}


int main(int argc, char** argv)
{
    std::string inputPath = (argc > 1) ? argv[1] : "../input.wav";
    std::string outputPath = (argc > 2) ? argv[2] : "../output.wav";

    Bins noisy;
    int sampleRate = 0;
    if (!read_wav(inputPath, noisy, sampleRate)) {  // after reading read_wav(...), noisy just seems like a vector of all samples (of the first channel)
        std::cerr << "Could not read " << inputPath << "\n";
        return 1;
    }
    if (noisy.size() < frameSize) {
        std::cerr << "Recording is shorter than one frame\n";
        return 1;
    }

    std::cout << noisy.size() << " samples at " << sampleRate << " Hz\n";

    Bins cleaned = denoise(noisy, sampleRate);

    std::cout << "Level change: "
              << 20 * std::log10(root_mean_square(cleaned) / root_mean_square(noisy)) << " dB\n";

    write_wav(outputPath, cleaned, sampleRate);
    return 0;
}