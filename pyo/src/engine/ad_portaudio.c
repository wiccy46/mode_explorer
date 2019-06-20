/**************************************************************************
 * Copyright 2009-2016 Olivier Belanger                                   *
 *                                                                        *
 * This file is part of pyo, a python module to help digital signal       *
 * processing script creation.                                            *
 *                                                                        *
 * pyo is free software: you can redistribute it and/or modify            *
 * it under the terms of the GNU Lesser General Public License as         *
 * published by the Free Software Foundation, either version 3 of the     *
 * License, or (at your option) any later version.                        *
 *                                                                        *
 * pyo is distributed in the hope that it will be useful,                 *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of         *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the          *
 * GNU Lesser General Public License for more details.                    *
 *                                                                        *
 * You should have received a copy of the GNU Lesser General Public       *
 * License along with pyo.  If not, see <http://www.gnu.org/licenses/>.   *
 *************************************************************************/

#include "ad_portaudio.h"

/* Audio server's functions. */

static void portaudio_assert(PaError ecode, const char* cmdName) {
    if (ecode != paNoError) {
        const char* eText = Pa_GetErrorText(ecode);
        if (!eText) {
            eText = "???";
        }
        printf("portaudio error in %s: %s\n", cmdName, eText);
        Pa_Terminate();
    }
}

int
pa_callback_interleaved( const void *inputBuffer, void *outputBuffer,
                            unsigned long framesPerBuffer,
                            const PaStreamCallbackTimeInfo* timeInfo,
                            PaStreamCallbackFlags statusFlags,
                            void *arg )
{
    float *out = (float *)outputBuffer;
    Server *server = (Server *) arg;

    assert(framesPerBuffer == server->bufferSize);
    int i, j, bufchnls, index1, index2;

    /* avoid unused variable warnings */
    (void) timeInfo;
    (void) statusFlags;

    if (server->withPortMidi == 1) {
        pyoGetMidiEvents(server);
    }

    if (server->duplex == 1) {
        float *in = (float *)inputBuffer;
        bufchnls = server->ichnls + server->input_offset;
        for (i=0; i<server->bufferSize; i++) {
            index1 = i * server->ichnls;
            index2 = i * bufchnls + server->input_offset;
            for (j=0; j<server->ichnls; j++) {
                server->input_buffer[index1+j] = (MYFLT)in[index2+j];
            }
        }
    }

    Server_process_buffers(server);
    bufchnls = server->nchnls + server->output_offset;
    for (i=0; i<server->bufferSize; i++) {
        index1 = i * server->nchnls;
        index2 = i * bufchnls + server->output_offset;
        for (j=0; j<server->nchnls; j++) {
            out[index2+j] = (float) server->output_buffer[index1+j];
        }
    }
    server->midi_count = 0;

#ifdef _OSX_
    if (server->server_stopped == 1)
        return paComplete;
    else
#endif
        return paContinue;
}

int
pa_callback_nonInterleaved( const void *inputBuffer, void *outputBuffer,
                        unsigned long framesPerBuffer,
                        const PaStreamCallbackTimeInfo* timeInfo,
                        PaStreamCallbackFlags statusFlags,
                        void *arg )
{
    float **out = (float **)outputBuffer;
    Server *server = (Server *) arg;

    assert(framesPerBuffer == server->bufferSize);
    int i, j;

    /* avoid unused variable warnings */
    (void) timeInfo;
    (void) statusFlags;

    if (server->withPortMidi == 1) {
        pyoGetMidiEvents(server);
    }

    if (server->duplex == 1) {
        float **in = (float **)inputBuffer;
        for (i=0; i<server->bufferSize; i++) {
            for (j=0; j<server->ichnls; j++) {
                server->input_buffer[(i*server->ichnls)+j] = (MYFLT)in[j+server->input_offset][i];
            }
        }
    }

    Server_process_buffers(server);
    for (i=0; i<server->bufferSize; i++) {
        for (j=0; j<server->nchnls; j++) {
            out[j+server->output_offset][i] = (float) server->output_buffer[(i*server->nchnls)+j];
        }
    }
    server->midi_count = 0;

#ifdef _OSX_
    if (server->server_stopped == 1)
        return paComplete;
    else
#endif
        return paContinue;
}

int
Server_pa_init(Server *self)
{
    PaError err;
    PaStreamParameters outputParameters;
    PaStreamParameters inputParameters;
    PaDeviceIndex n, inDevice, outDevice;
    const PaDeviceInfo *deviceInfo;
    PaHostApiIndex hostIndex;
    const PaHostApiInfo *hostInfo;
    PaHostApiTypeId hostId;
    PaSampleFormat sampleFormat;
    PaStreamCallback *streamCallback;

    err = Pa_Initialize();
    portaudio_assert(err, "Pa_Initialize");

    n = Pa_GetDeviceCount();
    if (n < 0) {
        portaudio_assert(n, "Pa_GetDeviceCount");
    }

    PyoPaBackendData *be_data = (PyoPaBackendData *) malloc(sizeof(PyoPaBackendData *));
    self->audio_be_data = (void *) be_data;

    if (self->output == -1)
        outDevice = Pa_GetDefaultOutputDevice(); /* default output device */
    else
        outDevice = (PaDeviceIndex) self->output; /* selected output device */
    if (self->input == -1)
        inDevice = Pa_GetDefaultInputDevice(); /* default input device */
    else
        inDevice = (PaDeviceIndex) self->input; /* selected input device */

    /* Retrieve host api id and define sample and callback format*/
    deviceInfo = Pa_GetDeviceInfo(outDevice);
    hostIndex = deviceInfo->hostApi;
    hostInfo = Pa_GetHostApiInfo(hostIndex);
    hostId = hostInfo->type;
    if (hostId == paASIO) {
        Server_debug(self, "Portaudio uses non-interleaved callback.\n");
        sampleFormat = paFloat32 | paNonInterleaved;
        streamCallback = pa_callback_nonInterleaved;
    }
    else if (hostId == paALSA) {
        Server_debug(self, "Portaudio uses interleaved callback.\n");
        Server_debug(self, "Using ALSA, if no input/output devices are specified, force to devices 0.\n");
        if (self->input == -1 && self->output == -1) {
            self->input = self->output = 0;
            inDevice = outDevice = (PaDeviceIndex) 0;
        }
        sampleFormat = paFloat32;
        streamCallback = pa_callback_interleaved;
    }
    else {
        Server_debug(self, "Portaudio uses interleaved callback.\n");
        sampleFormat = paFloat32;
        streamCallback = pa_callback_interleaved;
    }


    /* setup output and input streams */
    memset(&outputParameters, 0, sizeof(outputParameters));
    outputParameters.device = outDevice;
    outputParameters.channelCount = self->nchnls + self->output_offset;
    outputParameters.sampleFormat = sampleFormat;
    outputParameters.suggestedLatency = Pa_GetDeviceInfo( outputParameters.device )->defaultHighOutputLatency;
    outputParameters.hostApiSpecificStreamInfo = NULL;

    if (self->duplex == 1) {
        memset(&inputParameters, 0, sizeof(inputParameters));
        inputParameters.device = inDevice;
        inputParameters.channelCount = self->ichnls + self->input_offset;
        inputParameters.sampleFormat = sampleFormat;
        inputParameters.suggestedLatency = Pa_GetDeviceInfo( inputParameters.device )->defaultHighInputLatency ;
        inputParameters.hostApiSpecificStreamInfo = NULL;
    }

    if (self->input == -1 && self->output == -1) {
        if (self->duplex == 1)
            err = Pa_OpenDefaultStream(&be_data->stream,
                                       self->ichnls + self->input_offset,
                                       self->nchnls + self->output_offset,
                                       sampleFormat,
                                       self->samplingRate,
                                       self->bufferSize,
                                       streamCallback,
                                       (void *) self);
        else
            err = Pa_OpenDefaultStream(&be_data->stream,
                                       0,
                                       self->nchnls + self->output_offset,
                                       sampleFormat,
                                       self->samplingRate,
                                       self->bufferSize,
                                       streamCallback,
                                       (void *) self);
    }
    else {
        if (self->duplex == 1)
            err = Pa_OpenStream(&be_data->stream,
                                &inputParameters,
                                &outputParameters,
                                self->samplingRate,
                                self->bufferSize,
                                paNoFlag,
                                streamCallback,
                                (void *) self);
        else
            err = Pa_OpenStream(&be_data->stream,
                                (PaStreamParameters *) NULL,
                                &outputParameters,
                                self->samplingRate,
                                self->bufferSize,
                                paNoFlag,
                                streamCallback,
                                (void *) self);
    }
    portaudio_assert(err, "Pa_OpenStream");
    if (err < 0) {
        Server_error(self, "Portaudio error: %s", Pa_GetErrorText(err));
        return -1;
    }
    return 0;
}

int
Server_pa_deinit(Server *self)
{
    PaError err;
    PyoPaBackendData *be_data = (PyoPaBackendData *) self->audio_be_data;

    if (Pa_IsStreamActive(be_data->stream) || ! Pa_IsStreamStopped(be_data->stream)) {
        self->server_started = 0;
        err = Pa_AbortStream(be_data->stream);
        portaudio_assert(err, "Pa_AbortStream");
    }

    err = Pa_CloseStream(be_data->stream);
    portaudio_assert(err, "Pa_CloseStream");

    err = Pa_Terminate();
    portaudio_assert(err, "Pa_Terminate");

    free(self->audio_be_data);
    return err;
}

int
Server_pa_start(Server *self)
{
    PaError err;
    PyoPaBackendData *be_data = (PyoPaBackendData *) self->audio_be_data;

    if (Pa_IsStreamActive(be_data->stream) || ! Pa_IsStreamStopped(be_data->stream)) {
        err = Pa_AbortStream(be_data->stream);
        portaudio_assert(err, "Pa_AbortStream");
    }
    err = Pa_StartStream(be_data->stream);
    portaudio_assert(err, "Pa_StartStream");
    return err;
}

int
Server_pa_stop(Server *self)
{
    PyoPaBackendData *be_data = (PyoPaBackendData *) self->audio_be_data;

    if (Pa_IsStreamActive(be_data->stream) || ! Pa_IsStreamStopped(be_data->stream)) {
#ifndef _OSX_
        PaError err = Pa_AbortStream(be_data->stream);
        portaudio_assert(err, "Pa_AbortStream");
#endif
    }
    self->server_started = 0;
    self->server_stopped = 1;
    return 0;
}

/* Query functions. */

PyObject *
portaudio_get_version() {
    return PyInt_FromLong(Pa_GetVersion());
}

PyObject *
portaudio_get_version_text() {
    return PyString_FromString(Pa_GetVersionText());
}

PyObject *
portaudio_count_host_apis(){
    PaError err;
    PaHostApiIndex numApis;

    err = Pa_Initialize();
    if (err != paNoError) {
        portaudio_assert(err, "Pa_Initialize");
		Py_RETURN_NONE;
	}
	else {
        numApis = Pa_GetHostApiCount();
        if( numApis < 0 )
            portaudio_assert(numApis, "Pa_GetHostApiCount");
        return PyInt_FromLong(numApis);
    }
}

PyObject *
portaudio_list_host_apis(){
    PaError err;
    PaHostApiIndex n, i;

    err = Pa_Initialize();
    if (err != paNoError) {
        portaudio_assert(err, "Pa_Initialize");
	}
    else {
        n = Pa_GetHostApiCount();
        if (n < 0){
            portaudio_assert(n, "Pa_GetHostApiCount");
        }
        else {
            for (i=0; i < n; ++i){
                const PaHostApiInfo *info = Pa_GetHostApiInfo(i);
                assert(info);
                fprintf(stdout, "index: %i, id: %i, name: %s, num devices: %i, default in: %i, default out: %i\n", 
                        i, (int)info->type, info->name, (int)info->deviceCount, (int)info->defaultInputDevice, 
                        (int)info->defaultOutputDevice);
            }
        }
    }
    Py_RETURN_NONE;
}

PyObject *
portaudio_get_default_host_api(){
    PaError err;
    PaHostApiIndex i;

    err = Pa_Initialize();
    if (err != paNoError) {
        portaudio_assert(err, "Pa_Initialize");
		Py_RETURN_NONE;
	}
    else {
        i = Pa_GetDefaultHostApi();
        return PyInt_FromLong(i);
    }
}

PyObject *
portaudio_count_devices(){
    PaError err;
    PaDeviceIndex numDevices;

	err = Pa_Initialize();
    if (err != paNoError) {
        portaudio_assert(err, "Pa_Initialize");
		Py_RETURN_NONE;
	}
	else {
        numDevices = Pa_GetDeviceCount();
        if( numDevices < 0 )
            portaudio_assert(numDevices, "Pa_GetDeviceCount");
        return PyInt_FromLong(numDevices);
    }

}

PyObject *
portaudio_list_devices(){
    PaError err;
    PaDeviceIndex n, i;

	err = Pa_Initialize();
    if (err != paNoError) {
        portaudio_assert(err, "Pa_Initialize");
		Py_RETURN_NONE;
	}
    else {
        n = Pa_GetDeviceCount();
        if (n < 0){
            portaudio_assert(n, "Pa_GetDeviceCount");
        }
        else {
            printf("AUDIO devices:\n");
            for (i=0; i < n; ++i){
                const PaDeviceInfo *info = Pa_GetDeviceInfo(i);
                assert(info);

                if (info->maxInputChannels > 0)
                    fprintf(stdout, "%i: IN, name: %s, host api index: %i, default sr: %i Hz, latency: %f s\n", 
                            i, info->name, (int)info->hostApi, (int)info->defaultSampleRate, 
                            (float)info->defaultLowInputLatency);

                if (info->maxOutputChannels > 0)
                    fprintf(stdout, "%i: OUT, name: %s, host api index: %i, default sr: %i Hz, latency: %f s\n", 
                            i, info->name, (int)info->hostApi, (int)info->defaultSampleRate, 
                            (float)info->defaultLowOutputLatency);
            }
            printf("\n");
        }
    }
    Py_RETURN_NONE;
}

PyObject *
portaudio_get_devices_infos(){
    PaError err;
    PaDeviceIndex n, i;
    PyObject *inDict, *outDict, *tmpDict;
    inDict = PyDict_New();
    outDict = PyDict_New();

	err = Pa_Initialize();
    if (err != paNoError) {
        portaudio_assert(err, "Pa_Initialize");
		Py_RETURN_NONE;
	}
    else {
        n = Pa_GetDeviceCount();
        if (n < 0){
            portaudio_assert(n, "Pa_GetDeviceCount");
            Py_RETURN_NONE;
        }
        else {
            for (i=0; i < n; ++i){
                const PaDeviceInfo *info = Pa_GetDeviceInfo(i);
                assert(info);
                tmpDict = PyDict_New();
                if (info->maxInputChannels > 0) {
                    PyDict_SetItemString(tmpDict, "name", PyString_FromString(info->name));
                    PyDict_SetItemString(tmpDict, "host api index", PyInt_FromLong((int)info->hostApi));
                    PyDict_SetItemString(tmpDict, "default sr", PyInt_FromLong((int)info->defaultSampleRate));
                    PyDict_SetItemString(tmpDict, "latency", PyFloat_FromDouble((float)info->defaultLowInputLatency));
                    PyDict_SetItem(inDict, PyInt_FromLong(i), PyDict_Copy(tmpDict));
                }
                if (info->maxOutputChannels > 0) {
                    PyDict_SetItemString(tmpDict, "name", PyString_FromString(info->name));
                    PyDict_SetItemString(tmpDict, "host api index", PyInt_FromLong((int)info->hostApi));
                    PyDict_SetItemString(tmpDict, "default sr", PyInt_FromLong((int)info->defaultSampleRate));
                    PyDict_SetItemString(tmpDict, "latency", PyFloat_FromDouble((float)info->defaultLowOutputLatency));
                    PyDict_SetItem(outDict, PyInt_FromLong(i), PyDict_Copy(tmpDict));
                }
            }
            return Py_BuildValue("(OO)", inDict, outDict);
        }
    }
}

PyObject *
portaudio_get_output_devices(){
    PaError err;
    PaDeviceIndex n, i;

    PyObject *list, *list_index;
    list = PyList_New(0);
    list_index = PyList_New(0);

	err = Pa_Initialize();
    if (err != paNoError) {
        portaudio_assert(err, "Pa_Initialize");
		Py_RETURN_NONE;
	}
    else {
        n = Pa_GetDeviceCount();
        if (n < 0){
            portaudio_assert(n, "Pa_GetDeviceCount");
            Py_RETURN_NONE;
        }
        else {
            for (i=0; i < n; ++i){
                const PaDeviceInfo *info=Pa_GetDeviceInfo(i);
                assert(info);
                if (info->maxOutputChannels > 0){
                    PyList_Append(list, PyString_FromString(info->name));
                    PyList_Append(list_index, PyInt_FromLong(i));
                }
            }
            return Py_BuildValue("OO", list, list_index);
        }
    }
}

PyObject *
portaudio_get_output_max_channels(PyObject *self, PyObject *arg){
    PaError err;
    PaDeviceIndex n, i = PyInt_AsLong(arg);

	err = Pa_Initialize();
    if (err != paNoError) {
        portaudio_assert(err, "Pa_Initialize");
		Py_RETURN_NONE;
	}
    else {
        n = Pa_GetDeviceCount();
        if (n < 0){
            portaudio_assert(n, "Pa_GetDeviceCount");
            Py_RETURN_NONE;
        }
        else {
            const PaDeviceInfo *info=Pa_GetDeviceInfo(i);
            assert(info);
            return PyInt_FromLong(info->maxOutputChannels);
        }
    }
}

PyObject *
portaudio_get_input_max_channels(PyObject *self, PyObject *arg){
    PaError err;
    PaDeviceIndex n, i = PyInt_AsLong(arg);

	err = Pa_Initialize();
    if (err != paNoError) {
        portaudio_assert(err, "Pa_Initialize");
		Py_RETURN_NONE;
	}
    else {
        n = Pa_GetDeviceCount();
        if (n < 0){
            portaudio_assert(n, "Pa_GetDeviceCount");
            Py_RETURN_NONE;
        }
        else {
            const PaDeviceInfo *info=Pa_GetDeviceInfo(i);
            assert(info);
            return PyInt_FromLong(info->maxInputChannels);
        }
    }
}

PyObject *
portaudio_get_input_devices(){
    PaError err;
    PaDeviceIndex n, i;

    PyObject *list, *list_index;
    list = PyList_New(0);
    list_index = PyList_New(0);

	err = Pa_Initialize();
    if (err != paNoError) {
        portaudio_assert(err, "Pa_Initialize");
		Py_RETURN_NONE;
	}
    else {
        n = Pa_GetDeviceCount();
        if (n < 0){
            portaudio_assert(n, "Pa_GetDeviceCount");
            Py_RETURN_NONE;
        }
        else {
            for (i=0; i < n; ++i){
                const PaDeviceInfo *info=Pa_GetDeviceInfo(i);
                assert(info);
                if (info->maxInputChannels > 0){
                    PyList_Append(list, PyString_FromString(info->name));
                    PyList_Append(list_index, PyInt_FromLong(i));
                }
            }
            return Py_BuildValue("OO", list, list_index);
        }
    }
}

PyObject *
portaudio_get_default_input(){
    PaError err;
    PaDeviceIndex i;

	err = Pa_Initialize();
    if (err != paNoError) {
        portaudio_assert(err, "Pa_Initialize");
		Py_RETURN_NONE;
	}
    else {
        i = Pa_GetDefaultInputDevice();
        return PyInt_FromLong(i);
    }

}

PyObject *
portaudio_get_default_output(){
    PaError err;
    PaDeviceIndex i;

	err = Pa_Initialize();
    if (err != paNoError) {
        portaudio_assert(err, "Pa_Initialize");
		Py_RETURN_NONE;
	}
    else {
        i = Pa_GetDefaultOutputDevice();
        return PyInt_FromLong(i);

    }
}
