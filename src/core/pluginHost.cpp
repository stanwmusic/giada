/* -----------------------------------------------------------------------------
 *
 * Giada - Your Hardcore Loopmachine
 *
 * -----------------------------------------------------------------------------
 *
 * Copyright (C) 2010-2017 Giovanni A. Zuliani | Monocasual
 *
 * This file is part of Giada - Your Hardcore Loopmachine.
 *
 * Giada - Your Hardcore Loopmachine is free software: you can
 * redistribute it and/or modify it under the terms of the GNU General
 * Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * Giada - Your Hardcore Loopmachine is distributed in the hope that it
 * will be useful, but WITHOUT ANY WARRANTY; without even the implied
 * warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Giada - Your Hardcore Loopmachine. If not, see
 * <http://www.gnu.org/licenses/>.
 *
 * -------------------------------------------------------------------------- */


#ifdef WITH_VST


#include "../utils/log.h"
#include "../utils/fs.h"
#include "const.h"
#include "channel.h"
#include "plugin.h"
#include "pluginHost.h"


using std::vector;
using std::string;


namespace giada {
namespace pluginHost
{
namespace
{
juce::MessageManager *messageManager;

/* pluginFormat
 * Plugin format manager. */

juce::VSTPluginFormat pluginFormat;

/* knownPuginList
 * List of known (i.e. scanned) plugins. */

juce::KnownPluginList knownPluginList;

/* unknownPluginList
 * List of unrecognized plugins found in a patch. */

std::vector<std::string> unknownPluginList;

std::vector<Plugin*> masterOut;
std::vector<Plugin*> masterIn;

/* Audio|MidiBuffer
 * Dynamic buffers. */

juce::AudioBuffer<float> audioBuffer;

int samplerate;
int buffersize;

/* missingPlugins
 * If some plugins from any stack are missing. */

bool missingPlugins;

}; // {anonymous}


/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */


pthread_mutex_t mutex_midi;


/* -------------------------------------------------------------------------- */


void close()
{
  messageManager->deleteInstance();
}


/* -------------------------------------------------------------------------- */


void init(int _buffersize, int _samplerate)
{
  gu_log("[pluginHost::init] initialize with buffersize=%d, samplerate=%d\n",
    _buffersize, _samplerate);

  messageManager = juce::MessageManager::getInstance();
  audioBuffer.setSize(2, _buffersize);
  samplerate = _samplerate;
  buffersize = _buffersize;
  missingPlugins = false;
  //unknownPluginList.empty();
  loadList(gu_getHomePath() + G_SLASH + "plugins.xml");

  pthread_mutex_init(&mutex_midi, nullptr);
}


/* -------------------------------------------------------------------------- */


int scanDir(const string &dirpath, void (*callback)(float progress, void *p),
    void *p)
{
  gu_log("[pluginHost::scanDir] requested directory: '%s'\n", dirpath.c_str());
  gu_log("[pluginHost::scanDir] current plugins: %d\n", knownPluginList.getNumTypes());

  knownPluginList.clear();   // clear up previous plugins

  juce::VSTPluginFormat format;
  juce::FileSearchPath path(dirpath);
  juce::PluginDirectoryScanner scanner(knownPluginList, format, path,
      true, juce::File::nonexistent); // true: recursive

  bool cont = true;
  juce::String name;
  while (cont) {
    gu_log("[pluginHost::scanDir]   scanning '%s'\n", name.toRawUTF8());
    cont = scanner.scanNextFile(false, name);
    if (callback)
      callback(scanner.getProgress(), p);
  }

  gu_log("[pluginHost::scanDir] %d plugin(s) found\n", knownPluginList.getNumTypes());
  return knownPluginList.getNumTypes();
}


/* -------------------------------------------------------------------------- */


int saveList(const string &filepath)
{
  int out = knownPluginList.createXml()->writeToFile(juce::File(filepath), "");
  if (!out)
    gu_log("[pluginHost::saveList] unable to save plugin list to %s\n", filepath.c_str());
  return out;
}


/* -------------------------------------------------------------------------- */


int loadList(const string &filepath)
{
  juce::XmlElement *elem = juce::XmlDocument::parse(juce::File(filepath));
  if (elem) {
    knownPluginList.recreateFromXml(*elem);
    delete elem;
    return 1;
  }
  return 0;
}


/* -------------------------------------------------------------------------- */


Plugin *addPlugin(const string &fid, int stackType,
  pthread_mutex_t *mutex, Channel *ch)
{
  /* Get the proper stack to add the plugin to */

  vector <Plugin *> *pStack;
	pStack = getStack(stackType, ch);

  /* Initialize plugin */

  juce::PluginDescription *pd = knownPluginList.getTypeForFile(fid);
  if (!pd) {
    gu_log("[pluginHost::addPlugin] no plugin found with fid=%s!\n", fid.c_str());
    missingPlugins = true;
    unknownPluginList.push_back(fid);
    return nullptr;
  }

  juce::AudioPluginInstance *pi = pluginFormat.createInstanceFromDescription(*pd, samplerate, buffersize);
  if (!pi) {
    gu_log("[pluginHost::addPlugin] unable to create instance with fid=%s!\n", fid.c_str());
    missingPlugins = true;
    return nullptr;
  }
  gu_log("[pluginHost::addPlugin] plugin instance with fid=%s created\n", fid.c_str());

  Plugin *p = new Plugin(pi, samplerate, buffersize);

  /* Try to inject the plugin as soon as possible. */

  while (true) {
    if (pthread_mutex_trylock(mutex) != 0)
      continue;
    pStack->push_back(p);
    pthread_mutex_unlock(mutex);
    break;
  }

  gu_log("[pluginHost::addPlugin] plugin id=%s loaded (%s), stack type=%d, stack size=%d\n",
    fid.c_str(), p->getName().c_str(), stackType, pStack->size());

  return p;
}


/* -------------------------------------------------------------------------- */


Plugin *addPlugin(int index, int stackType, pthread_mutex_t *mutex,
  Channel *ch)
{
  juce::PluginDescription *pd = knownPluginList.getType(index);
  if (pd) {
    gu_log("[pluginHost::addPlugin] plugin found, uid=%s, name=%s...\n",
      pd->fileOrIdentifier.toStdString().c_str(), pd->name.toStdString().c_str());
    return addPlugin(pd->fileOrIdentifier.toStdString(), stackType, mutex, ch);
  }
  else {
    gu_log("[pluginHost::addPlugin] no plugins found at index=%d!\n", index);
    return nullptr;
  }
}


/* -------------------------------------------------------------------------- */


vector <Plugin *> *getStack(int stackType, Channel *ch)
{
	switch(stackType) {
		case MASTER_OUT:
			return &masterOut;
		case MASTER_IN:
			return &masterIn;
		case CHANNEL:
			return &ch->plugins;
		default:
			return nullptr;
	}
}


/* -------------------------------------------------------------------------- */


unsigned countPlugins(int stackType, Channel *ch)
{
	vector <Plugin *> *pStack = getStack(stackType, ch);
	return pStack->size();
}


/* -------------------------------------------------------------------------- */


int countAvailablePlugins()
{
  return knownPluginList.getNumTypes();
}


/* -------------------------------------------------------------------------- */


unsigned countUnknownPlugins()
{
  return unknownPluginList.size();
}


/* -------------------------------------------------------------------------- */


pluginHost::PluginInfo getAvailablePluginInfo(int i)
{
  juce::PluginDescription *pd = knownPluginList.getType(i);
  PluginInfo pi;
  pi.uid = pd->fileOrIdentifier.toStdString();
  pi.name = pd->name.toStdString();
  pi.category = pd->category.toStdString();
  pi.manufacturerName = pd->manufacturerName.toStdString();
  pi.format = pd->pluginFormatName.toStdString();
  pi.isInstrument = pd->isInstrument;
/*
  if (!p) {
    gu_log("[pluginHost::getAvailablePlugin] unable to create plugin instance!\n");
    return nullptr;
  }
  */
  return pi;
}


/* -------------------------------------------------------------------------- */


bool hasMissingPlugins()
{
  return missingPlugins;
};


/* -------------------------------------------------------------------------- */


string getUnknownPluginInfo(int i)
{
  return unknownPluginList.at(i);
}


/* -------------------------------------------------------------------------- */


void freeStack(int stackType, pthread_mutex_t *mutex, Channel *ch)
{
	vector <Plugin *> *pStack;
	pStack = getStack(stackType, ch);

	if (pStack->size() == 0)
		return;

	while (true) {
		if (pthread_mutex_trylock(mutex) != 0)
      continue;
		for (unsigned i=0; i<pStack->size(); i++)
      delete pStack->at(i);
		pStack->clear();
		pthread_mutex_unlock(mutex);
		break;
	}
  gu_log("[pluginHost::freeStack] stack type=%d freed\n", stackType);
}


/* -------------------------------------------------------------------------- */


void processStack(float *buffer, int stackType, Channel *ch)
{
	vector <Plugin *> *pStack = getStack(stackType, ch);

	/* empty stack, stack not found or mixer not ready: do nothing */

	if (pStack == nullptr || pStack->size() == 0)
		return;

	/* converting buffer from Giada to Juce */

	for (int i=0; i<buffersize; i++) {
    audioBuffer.setSample(0, i, buffer[i*2]);
    audioBuffer.setSample(1, i, buffer[(i*2)+1]);
	}

	/* Hardcore processing. At the end we swap input and output, so that he N-th
  plugin will process the result of the plugin N-1. Part of this loop must be
  guarded by mutexes, i.e. the MIDI process part. You definitely don't want
  a situation like the following one:
    processBlock(...)
    [a new midi event from kernelMidi thread]
    clearMidiBuffer()
  The midi event in between would be surely lost, deleted by clearMidiBuffer! */

	for (unsigned i=0; i<pStack->size(); i++) {
    Plugin *plugin = pStack->at(i);
		if (plugin->isSuspended() || plugin->isBypassed())
			continue;
    if (ch) { // ch might be null if stackType is MASTER_IN/OUT
      pthread_mutex_lock(&mutex_midi);
      plugin->process(audioBuffer, ch->getPluginMidiEvents());
      ch->clearMidiBuffer();
      pthread_mutex_unlock(&mutex_midi);
    }
    else {
      juce::MidiBuffer midiBuffer;  // empty buffer
      plugin->process(audioBuffer, midiBuffer);
    }
  }

	/* converting buffer from Juce to Giada. A note for the future: if we
	 * overwrite (=) (as we do now) it's SEND, if we add (+) it's INSERT. */

	for (int i=0; i<buffersize; i++) {
		buffer[i*2]     = audioBuffer.getSample(0, i);
		buffer[(i*2)+1] = audioBuffer.getSample(1, i);
	}
}


/* -------------------------------------------------------------------------- */


Plugin *getPluginByIndex(int index, int stackType, Channel *ch)
{
	vector <Plugin *> *pStack = getStack(stackType, ch);
	if (pStack->size() == 0)
		return nullptr;
	if ((unsigned) index >= pStack->size())
		return nullptr;
	return pStack->at(index);
}


/* -------------------------------------------------------------------------- */


int getPluginIndex(int id, int stackType, Channel *ch)
{
	vector <Plugin *> *pStack = getStack(stackType, ch);
	for (unsigned i=0; i<pStack->size(); i++)
		if (pStack->at(i)->getId() == id)
			return i;
	return -1;
}


/* -------------------------------------------------------------------------- */


void swapPlugin(unsigned indexA, unsigned indexB, int stackType,
  pthread_mutex_t *mutex, Channel *ch)
{
	vector <Plugin *> *pStack = getStack(stackType, ch);
	while (true) {
		if (pthread_mutex_trylock(mutex) != 0)
      continue;
		std::swap(pStack->at(indexA), pStack->at(indexB));
		pthread_mutex_unlock(mutex);
		gu_log("[pluginHost::swapPlugin] plugin at index %d and %d swapped\n", indexA, indexB);
		return;
	}
}


/* -------------------------------------------------------------------------- */


int freePlugin(int id, int stackType, pthread_mutex_t *mutex,
  Channel *ch)
{
	vector <Plugin *> *pStack = getStack(stackType, ch);
	for (unsigned i=0; i<pStack->size(); i++) {
    Plugin *pPlugin = pStack->at(i);
		if (pPlugin->getId() != id)
      continue;
		while (true) {
			if (pthread_mutex_trylock(mutex) != 0)
        continue;
			delete pPlugin;
			pStack->erase(pStack->begin() + i);
			pthread_mutex_unlock(mutex);
			gu_log("[pluginHost::freePlugin] plugin id=%d removed\n", id);
			return i;
		}
  }
	gu_log("[pluginHost::freePlugin] plugin id=%d not found\n", id);
  return -1;
}


/* -------------------------------------------------------------------------- */


void runDispatchLoop()
{
  messageManager->runDispatchLoopUntil(10);
  //gu_log("[pluginHost::runDispatchLoop] %d, hasStopMessageBeenSent=%d\n", r, messageManager->hasStopMessageBeenSent());
}


/* -------------------------------------------------------------------------- */


void freeAllStacks(vector <Channel*> *channels, pthread_mutex_t *mutex)
{
	freeStack(pluginHost::MASTER_OUT, mutex);
	freeStack(pluginHost::MASTER_IN, mutex);
	for (unsigned i=0; i<channels->size(); i++)
		freeStack(pluginHost::CHANNEL, mutex, channels->at(i));
  missingPlugins = false;
  unknownPluginList.clear();
}


/* -------------------------------------------------------------------------- */


int clonePlugin(Plugin *src, int stackType, pthread_mutex_t *mutex,
  Channel *ch)
{
	Plugin *p = addPlugin(src->getUniqueId(), stackType, mutex, ch);
	if (!p) {
		gu_log("[pluginHost::clonePlugin] unable to add new plugin to stack!\n");
		return 0;
	}

	for (int k=0; k<src->getNumParameters(); k++)
		p->setParameter(k, src->getParameter(k));

	return 1;
}


/* -------------------------------------------------------------------------- */


bool doesPluginExist(const string &fid)
{
  return pluginFormat.doesPluginStillExist(*knownPluginList.getTypeForFile(fid));
}


/* -------------------------------------------------------------------------- */


void sortPlugins(int method)
{
  switch (method) {
    case sortMethod::NAME:
      knownPluginList.sort(juce::KnownPluginList::SortMethod::sortAlphabetically, true);
      break;
    case sortMethod::CATEGORY:
      knownPluginList.sort(juce::KnownPluginList::SortMethod::sortByCategory, true);
      break;
    case sortMethod::MANUFACTURER:
      knownPluginList.sort(juce::KnownPluginList::SortMethod::sortByManufacturer, true);
      break;
    case sortMethod::FORMAT:
      knownPluginList.sort(juce::KnownPluginList::SortMethod::sortByFormat, true);
      break;
  }
}

}}; // giada::pluginHost::


#endif // #ifdef WITH_VST
