%% Compression and denoising
% Cleans the respiratory component (highest-SNR IC) from source separation
% (preprocess_ica_filter.m): spike removal, two-stage dynamic range
% compression, Wiener-filter denoising with a noise profile taken from the
% quietest 1 s, a 60 Hz high-pass and loudness normalization. Writes the
% result as .csv and .wav for segmentation and frameset creation.
%
% Requires the Signal Processing Toolbox and the Audio Toolbox (compressor).
% Run from the source_separation_classification folder.

clear all; clc; close all;

%% Parameters

fs = 4000;
subject = 'S01';
position = 'supine';
filename = 'recording_MIC_post.csv';  % name of the original recording
% Respiratory component written by preprocess_ica_filter.m
dataFilepath = fullfile('data', 'preprocessed', subject, position, strrep(filename, '.csv', '_respiratory.csv'));

exportFolder = fullfile('data', 'fully_prepared', subject, position);
exportPath = fullfile(exportFolder, filename);

sensitivity = 3;  % Wiener filter noise weighting


%% Load data

data = normAmp(readmatrix(dataFilepath));


%% Compression and noise removal

threeSigma = prctile(abs(data), 99.9);
data = spikeRemoval(data, 50, threeSigma);

dRC = compressor(20*log10(threeSigma), 10, "AttackTime", 0.05, "ReleaseTime", 0.05, "SampleRate", fs, "KneeWidth", 5);  % define compressor object
data = dRC(data);  % compression
threeSigma = prctile(abs(data), 99.75);
dRC = compressor(20*log10(threeSigma), 10, "AttackTime", 0.05, "ReleaseTime", 0.05, "SampleRate", fs, "KneeWidth", 5);
data = dRC(data);

[nStart, nEnd, meanVal] = extractNoise(data, fs);  % noise profile from the quietest 1 s window

[data, audioMag] = denoise(data, nStart, nEnd, fs, sensitivity);

data = highpass(movmedian(data, 3), 60, fs, "Steepness", 0.5, "StopbandAttenuation", 10);
data = spikeRemoval(data, 10, prctile(abs(data), 99.9));
data = normalizeLoudness(normalizeLoudness(data));  % smoothing and audio normalization
data = spikeRemoval(data, 50, prctile(data, 99.7));
if max(abs(data)) > 1
    data = normAmp(data);
end
%data = min(1, max(data, -1));


%% Export

if ~isfolder(exportFolder)
    mkdir(exportFolder);
end

csvExportPath = exportPath;
wavExportPath = strcat(exportPath(1:end-4), '.wav');

writematrix(data, csvExportPath);
audiowrite(wavExportPath, data, fs);


%% Helper functions

function signal = spikeRemoval(data, width, amplitude)
% SPIKEREMOVAL removes spikes by smoothing the signal where the mean value
% over a specified width is above the given amplitude. Smoothing is done by
% moving median over a window size of width/2.

% Call format: signal = spikeRemoval(data, width, amplitude)
% width is given as an integer number of samples
    signal = data;
    for j = 1:length(data) - width
        avg = mean(data(j:min(length(data), j+width)));
        if avg > amplitude
            startInd = max(1, j - int32(1.5*width));
            endInd = min(length(data), j + int32(1.5*width));
            signal(startInd:endInd) = movmedian(data(startInd:endInd), int32(width/2));
        end
    end
end


function [signal, audioMag] = denoise(data, noiseStart, noiseEnd, fs, sensitivity)
% DENOISE applies denoising algorithm based upon a Wiener filter. The
% function takes in noiseStart and noiseEnd specified as a sample numbers
% to create a noise profile to compare to the signal.
% Gain calculation: G(m,ω) = P_S(m,ω)/(P_S(m,ω) + P_N(m,ω)), where m is
% window number and ω is the frequency bin.

% Sensitivity determines the aggressiveness of gain reduction.

    winLen = 256;
    overlap = 192; % 75% overlap
    nfft = 256;
    win = hann(winLen, 'periodic');

    % Short Time Fourier Transform for signal and noise profile
    audioProfile = stft(data, fs, 'Window', win, 'OverlapLength', overlap, 'FFTLength', nfft);
    audioMag = abs(audioProfile);
    audioPhase = angle(audioProfile);

    noiseProfile = stft(data(noiseStart:noiseEnd), fs, 'Window', win, 'OverlapLength', overlap, 'FFTLength', nfft);
    noiseProfile = repmat(noiseProfile, 1, ceil(size(audioMag, 2)/size(noiseProfile, 2)));
    noiseProfile = noiseProfile(:, 1:size(audioMag, 2));

    % Gain calculation via estimated power spectra and Wiener gain
    audioPSD = audioMag.^2;
    noisePSD = noiseProfile.^2;
    movmean(noisePSD, 7, 2);  % note: result not assigned, noisePSD is unchanged
    movmean(noisePSD, 7, 1);

    gain = audioPSD ./ (audioPSD + sensitivity*noisePSD);
    gain = min(1, max(0.15, gain));  % applying spectral floor of minimum 0.15 gain and maximum unity gain
    gain = movmean(gain, 5, 2);  % smooth in both frequency and time
    gain = movmean(gain, 5, 1);

    % Gain application and reconstruction of signal
    audioSpectrumClean = audioMag .* gain .* exp(1i * audioPhase);
    signal = real(istft(audioSpectrumClean, fs, 'Window', win, 'OverlapLength', overlap, 'FFTLength', nfft));

end


function [nStart, nEnd, meanVal] = extractNoise(vector, N)
% EXTRACTNOISE extracts the limits of a window of N samples where the mean of the window
% is minimum in the specified vector.

% Call format: [nStart, nEnd, meanVal] = extractNoise(vector, N)
    vector = abs(vector);
    minVal = max(vector);
    nStart = 0;
    nEnd = 0;
    for j = 1:length(vector) - N
        meanVal = mean(vector(j:j+N));
        if meanVal <= minVal
            minVal = meanVal;
            nStart = j;
            nEnd = j + N;
        end
    end
    meanVal = minVal;
end


function normSig = normalizeLoudness(data)
% NORMALIZELOUDNESS normalizes audio to -20 LUFS. Above 0.85 magnitude,
% nonlinear compression is applied.

    targetLUFS = -20;

    LUFS = -0.691 + 10*log10(mean(data.^2));
    gain = 10.^((targetLUFS - LUFS)/20);

    normSig = data * gain;
    normSig(normSig < -0.85) = -0.74393 - sqrt(-0.075*(normSig(normSig < -0.85)+0.7));
    normSig(normSig > 0.85) = 0.74393 + sqrt(0.075*(normSig(normSig > 0.85)-0.7));

end


function normSig = normAmp(vector)
% NORMAMP normalizes the amplitude of the input vector.
    normSig = vector / max(abs(vector));
end