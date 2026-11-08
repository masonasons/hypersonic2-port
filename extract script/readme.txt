This is a readme for this completely vibe coded script to export all of the samples from Hypersonic 2 as wav.
The script is written in Python. The only dependencies you need to run it are a Python installation and the numpy package.
Some background on how the script works (very non technical):
Inside the samples directory in your Hypersonic 2 Content, there should be 56 files. Each file contains the sample data for an individual category of sounds. In addition, each file also contains information about its samples such as length, sampling rate, key and velocity information, loop points, fine tuning and more. Thus, the script does two things: First it locates samples, decompressing them if necessary, and converts them to wav. Then it writes the aforementioned properties in an index.csv file.

Can you have the script convert to an instrument format like SoundFont, SFZ, Kontakt...
Short answer, no.
Long answer: To my knowledge, the sample files do not contain enough information to build actual instruments. They just contain basic sample properties and mapping information. Envelopes, effects and other controls of that nature most likely live in the patches, and extracting that information would be a whole other project.
Also, many of the categories have multiple sample sets in them. For instance the grand pianos file has two pianos. While it might be possible to work out an automated way to separate them, I decided to leave that for another day and to keep this tool as a sample extractor.
If you want to convert to an instrument format, the best thing to do at the moment would be to make an index.csv converter. You would then have to edit the resulting instrument to taste.

Notes about sample formats and clipping:
Hypersonic's audio engine outputs float, and the codec used on most of the samples can handle float at least to some degree. Because of this, I decided to make 32 bit float the default output format for the wavs. Using 16 or 24 bit pcm would introduce some clipping on many samples, and on a handful of samples the clipping would be severe. The loudest sample peaked at almost +10 dB! Claude double-checked it though, and the decoder isn't having a fit. The sample is just stupidly loud.
What potentially makes things more confusing is that the banks contain a field for gain adjustment. At first, I was hopeful that this would be used to prevent clipping, but instead, it appears to only balance the sample levels for playback.
By default, the wavs are decoded as is, with the gain adjustments from the bank documented in index.csv. There is, however, an option in the extractor to apply the bank's gain adjustments to the wav files.
The script will check each sample for clipping before exporting it. If a sample would clip, its volume is automatically turned down. This protection only applies if you change the default export format to 16 or 24 bit.
The clipping protection comes after gain adjustments. For example, if a sample decodes with a peak at +3 dB, but the bank adjusts the gain by -2 dB, the clipping detection will see it as a peak at +1.

Running the script:
python milan_decode.py [options] input_file
Input_file is one or more files from the samples folder from your Hypersonic 2 Content. By default, the script creates a folder in your current working directory and exports the samples there. You can change the output folder with the -o option.
By default, the script prints information about every detected sample, then wraps it up with a final report when the decoding is done. If you use the -q option though, then the samples are extracted silently and only the final report is printed.
Some other options:
-f n: set wav format for the export. N can either be pcm16, pcm24 or float32. Default is float32.
--stats-only: Checks the samples and prints the usual information, but does not extract them.
--apply-zone-gain: apply the bank's gain adjustments to the exported samples, instead of just documenting them in index.csv.
--no-numbers: By default the script puts sample numbers in file names, which I generally prefer since it keeps samples organized. The --no-numbers option removes sample numbers from file names.
-g n: lowers all samples by n dB. Useful both to adjust every sample by a constant amount, and to combat clipping when saving wavs as 16 or 24 bit.
--no-prevent-clipping: Disables the clipping protection.
Note: Clipping protection is always disabled when exporting as 32 bit float.
You can read about more settings, as well as aliases for the above options, by running the script with no arguments or -h.

Known issues:
The files 040 Acoustic Guitars and 110 Attack Partials may complain about truncated data. Claude believes the files themselves may be at fault. The script will export what it can.
The script doesn't break any speed records. A full run on Hypersonic 2's Content takes about 10 minutes on my machine.
If for some reason the script on your machine reports skips or seek errors, or outright refuses to decode a file, let me know. Your files may be different from mine. That's worth an investigation.
And that's about it! Have fun!