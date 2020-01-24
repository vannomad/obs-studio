#include "volume-control.hpp"
#include "qt-wrappers.hpp"
#include "obs-app.hpp"
#include "window-basic-main.hpp"
#include "mute-checkbox.hpp"
#include "slider-ignorewheel.hpp"
#include "slider-absoluteset-style.hpp"
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QPainter>
#include <QStyleFactory>

using namespace std;

#define CLAMP(x, min, max) ((x) < (min) ? (min) : ((x) > (max) ? (max) : (x)))
#define FADER_PRECISION 4096.0

QWeakPointer<VolumeMeterTimer> VolumeMeter::updateTimer;

void VolControl::OBSVolumeChanged(void *data, float db)
{
	Q_UNUSED(db);
	VolControl *volControl = static_cast<VolControl *>(data);

	QMetaObject::invokeMethod(volControl, "VolumeChanged");
}

void VolControl::OBSVolumeLevel(void *data,
				const float magnitude[MAX_AUDIO_CHANNELS],
				const float peak[MAX_AUDIO_CHANNELS],
				const float inputPeak[MAX_AUDIO_CHANNELS])
{
	VolControl *volControl = static_cast<VolControl *>(data);

	volControl->volMeter->setLevels(magnitude, peak, inputPeak);
}

void VolControl::OBSVolumeMuted(void *data, calldata_t *calldata)
{
	VolControl *volControl = static_cast<VolControl *>(data);
	bool muted = calldata_bool(calldata, "muted");

	QMetaObject::invokeMethod(volControl, "VolumeMuted",
				  Q_ARG(bool, muted));
}

void VolControl::OBSSourceMixersChanged(void *data, calldata_t *calldata)
{
	VolControl *volControl = static_cast<VolControl *>(data);
	uint32_t mixers = (uint32_t)calldata_int(calldata, "mixers");
	QMetaObject::invokeMethod(volControl, "SourceMixersChanged",
				  Q_ARG(uint32_t, mixers));
}

void VolControl::OBSSourceMonitoringChanged(void *data, calldata_t *calldata)
{
	VolControl *volControl = static_cast<VolControl *>(data);
	int type = (int)calldata_int(calldata, "mon_type");
	QMetaObject::invokeMethod(volControl, "SourceMonitoringTypeChanged",
				  Q_ARG(int, type));
}

void VolControl::VolumeChanged()
{
	slider->blockSignals(true);
	slider->setValue(
		(int)(obs_fader_get_deflection(obs_fader) * FADER_PRECISION));
	slider->blockSignals(false);

	updateText();
}

void VolControl::VolumeMuted(bool muted)
{
	if (mute->isChecked() != muted)
		mute->setChecked(muted);
}

void VolControl::SetMuted(bool checked)
{
	obs_source_set_muted(source, checked);

	if (mutePtr)
		*mutePtr = checked;
}

void VolControl::SetMon(bool checked)
{
	obs_monitoring_type mt;
	if (!checked)
		mt = OBS_MONITORING_TYPE_NONE;
	else if (obs_source_get_sends(source))
		mt = OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT;
	else
		mt = OBS_MONITORING_TYPE_MONITOR_ONLY;

	obs_source_set_monitoring_type(source, mt);
	obs_source_set_track_active(source);
}

void VolControl::SetStream(bool checked)
{
	OBSBasic *main = reinterpret_cast<OBSBasic *>(App()->GetMainWindow());
	int index = checked ? track_index : 0;
	config_set_int(main->Config(), "AdvOut", "TrackIndex", index + 1);
	main->ResetOutputs();
	config_save_safe(main->Config(), "tmp", nullptr);
	for (auto vol : main->GetMasterVol()) {
		if (vol->track_index != index)
			vol->stream->setChecked(false);
		else
			vol->stream->setChecked(true);
	}
	/* The monitoring API unfortunately links it to output.
	 * The next call is required to update the monitoring_type.
	 * Remove it if API is reworked.
	 * See also SetRec function. This is used for monitor hotkey.
	 */
	SetMon(mon->isChecked());
}

void VolControl::SetRec(bool checked)
{
	Q_UNUSED(checked);
	OBSBasic *main = reinterpret_cast<OBSBasic *>(App()->GetMainWindow());
	std::string RecMode =
		config_get_string(main->Config(), "AdvOut", "RecType");
	bool isStandard = RecMode.compare("Standard") == 0;
	int recbitmask;
	if (isStandard)
		recbitmask =
			config_get_int(main->Config(), "AdvOut", "RecTracks");
	else
		recbitmask = config_get_int(main->Config(), "AdvOut",
					    "FFAudioMixes");

	int newbitmask = recbitmask ^ (1 << track_index);
	if (!newbitmask) {
		newbitmask = 1 << 0;
		for (auto vol : main->GetMasterVol()) {
			if (vol->track_index != 0)
				vol->rec->setChecked(false);
			else
				vol->rec->setChecked(true);
		}
	}
	if (isStandard)
		config_set_int(main->Config(), "AdvOut", "RecTracks",
			       newbitmask);
	else
		config_set_int(main->Config(), "AdvOut", "FFAudioMixes",
			       newbitmask);

	main->ResetOutputs();
	config_save_safe(main->Config(), "tmp", nullptr);
	SetMon(mon->isChecked());
}

void VolControl::enableStreamButton(bool show)
{
	stream->setEnabled(show);
}

void VolControl::enableRecButton(bool show)
{
	rec->setEnabled(show);
}

void VolControl::checkMonButton(bool check)
{
	mon->setChecked(check);
}

void VolControl::showMonitoringButton(bool show)
{
	mon->setHidden(!show);
}

void VolControl::SourceMonitoringTypeChanged(int type)
{
	OBSBasic *main = reinterpret_cast<OBSBasic *>(App()->GetMainWindow());
	if ((enum obs_monitoring_type)type != OBS_MONITORING_TYPE_NONE)
		main->SelectiveMonitoring(track_index);
}

void VolControl::SourceMixersChanged(uint32_t mixers)
{
	track1->setChecked(mixers & (1 << 0));
	track2->setChecked(mixers & (1 << 1));
	track3->setChecked(mixers & (1 << 2));
	track4->setChecked(mixers & (1 << 3));
	track5->setChecked(mixers & (1 << 4));
	track6->setChecked(mixers & (1 << 5));
}

void VolControl::setMixer(obs_source_t *source, const int mixerIdx,
			  const bool checked)
{
	uint32_t mixers = obs_source_get_audio_mixers(source);
	uint32_t new_mixers = mixers;

	if (checked)
		new_mixers |= (1 << mixerIdx);
	else
		new_mixers &= ~(1 << mixerIdx);

	obs_source_set_audio_mixers(source, new_mixers);
}

void VolControl::track1Changed(bool checked)
{
	setMixer(source, 0, checked);
}

void VolControl::track2Changed(bool checked)
{
	setMixer(source, 1, checked);
}

void VolControl::track3Changed(bool checked)
{
	setMixer(source, 2, checked);
}

void VolControl::track4Changed(bool checked)
{
	setMixer(source, 3, checked);
}

void VolControl::track5Changed(bool checked)
{
	setMixer(source, 4, checked);
}

void VolControl::track6Changed(bool checked)
{
	setMixer(source, 5, checked);
}

void VolControl::showTracksButtons(bool show)
{
	track1->setHidden(!show);
	track2->setHidden(!show);
	track3->setHidden(!show);
	track4->setHidden(!show);
	track5->setHidden(!show);
	track6->setHidden(!show);
}

void VolControl::SliderChanged(int vol)
{
	obs_fader_set_deflection(obs_fader, float(vol) / FADER_PRECISION);
	updateText();
}

void VolControl::updateText()
{
	QString text;
	float db = obs_fader_get_db(obs_fader);

	if (db < -96.0f)
		text = "-inf dB";
	else
		text = QString::number(db, 'f', 1).append(" dB");

	volLabel->setText(text);

	bool muted = obs_source_muted(source);
	const char *accTextLookup = muted ? "VolControl.SliderMuted"
					  : "VolControl.SliderUnmuted";

	QString sourceName = obs_source_get_name(source);
	QString accText = QTStr(accTextLookup).arg(sourceName, db);

	slider->setAccessibleName(accText);
}

QString VolControl::GetName() const
{
	return nameLabel->text();
}

void VolControl::SetName(const QString &newName)
{
	nameLabel->setText(newName);
}

void VolControl::EmitConfigClicked()
{
	emit ConfigClicked();
}

void VolControl::SetMeterDecayRate(qreal q)
{
	volMeter->setPeakDecayRate(q);
}

void VolControl::setPeakMeterType(enum obs_peak_meter_type peakMeterType)
{
	volMeter->setPeakMeterType(peakMeterType);
}

VolControl::VolControl(OBSSource source_, bool *mutePtr, bool showConfig,
		       bool vertical, bool showMon, bool showTracks,
		       int trackIndex)
	: source(std::move(source_)),
	  levelTotal(0.0f),
	  levelCount(0.0f),
	  obs_fader(obs_fader_create(OBS_FADER_LOG)),
	  obs_volmeter(obs_volmeter_create(OBS_FADER_LOG)),
	  mutePtr(mutePtr),
	  vertical(vertical)
{
	nameLabel = new QLabel();
	volLabel = new QLabel();
	mute = new MuteCheckBox();
	uint32_t mixers = 0xFF;
#if defined(_WIN32) || defined(__APPLE__) || HAVE_PULSEAUDIO
	mon = new MonCheckBox();
#endif
	if (trackIndex < 0) {
		track1 = new TracksCheckBox();
		track2 = new TracksCheckBox();
		track3 = new TracksCheckBox();
		track4 = new TracksCheckBox();
		track5 = new TracksCheckBox();
		track6 = new TracksCheckBox();
		track1->setGeometry(0, 0, 16, 16);
		track2->setGeometry(0, 0, 16, 16);
		track3->setGeometry(0, 0, 16, 16);
		track4->setGeometry(0, 0, 16, 16);
		track5->setGeometry(0, 0, 16, 16);
		track6->setGeometry(0, 0, 16, 16);
		if (!showTracks) {
			track1->setHidden(!showTracks);
			track2->setHidden(!showTracks);
			track3->setHidden(!showTracks);
			track4->setHidden(!showTracks);
			track5->setHidden(!showTracks);
			track6->setHidden(!showTracks);
		}
	}
	track_index = trackIndex;
	OBSBasic *main = reinterpret_cast<OBSBasic *>(App()->GetMainWindow());
	QString sourceName;

	/* set the name of the meter */
	if (trackIndex >= 0) {
		stream = new StreamCheckBox();
		rec = new RecCheckBox();

		std::string trackNum =
			"Track" + std::to_string(trackIndex + 1) + "Name";
		const char *nameAdv = config_get_string(
			main->Config(), "AdvOut", trackNum.c_str());
		trackNum = "Track " + std::to_string(trackIndex + 1);
		const char *name;
		if (nameAdv) {
			std::string nameStr = nameAdv;
			name = nameStr.compare("") == 0 ? trackNum.c_str()
							: nameAdv;
		} else {
			name = trackNum.c_str();
		}
		sourceName = QString::fromUtf8(name);
	} else {
		sourceName = obs_source_get_name(source);
	}
	setObjectName(sourceName);

	if (showConfig) {
		config = new QPushButton(this);
		config->setProperty("themeID", "configIconSmall");
		config->setFlat(true);
		config->setSizePolicy(QSizePolicy::Maximum,
				      QSizePolicy::Maximum);
		config->setMaximumSize(16, 16);
		config->setAutoDefault(false);

		config->setAccessibleName(
			QTStr("VolControl.Properties").arg(sourceName));

		connect(config, &QAbstractButton::clicked, this,
			&VolControl::EmitConfigClicked);
	}

	QVBoxLayout *mainLayout = new QVBoxLayout;
	mainLayout->setContentsMargins(4, 2, 4, 2);
	mainLayout->setSpacing(2);

	if (vertical) {
		QHBoxLayout *nameLayout = new QHBoxLayout;
		QHBoxLayout *volLayout = new QHBoxLayout;
		QHBoxLayout *meterLayout = new QHBoxLayout;
		QHBoxLayout *controlLayout = new QHBoxLayout;
		QVBoxLayout *rightLayout = new QVBoxLayout;

		volMeter = new VolumeMeter(nullptr, obs_volmeter, true);
		slider = new SliderIgnoreScroll(Qt::Vertical);

		nameLayout->setAlignment(Qt::AlignLeft);
		volLayout->setAlignment(Qt::AlignLeft);
		meterLayout->setAlignment(Qt::AlignLeft);
		controlLayout->setAlignment(Qt::AlignLeft);

		nameLayout->setContentsMargins(0, 0, 0, 0);
		nameLayout->setSpacing(0);
		nameLayout->addWidget(nameLabel);

		volLayout->setContentsMargins(0, 0, 0, 0);
		volLayout->setSpacing(0);
		volLayout->addWidget(volLabel);

		rightLayout->setAlignment(Qt::AlignTop);
		rightLayout->setContentsMargins(0, 0, 0, 0);
#ifndef __APPLE__
		rightLayout->setSpacing(5);
#else
		rightLayout->setSpacing(12);
#endif

#if defined(_WIN32) || defined(__APPLE__) || HAVE_PULSEAUDIO
		rightLayout->addWidget(mon);
#endif
		if (trackIndex < 0) {
			rightLayout->addWidget(track1);
			rightLayout->addWidget(track2);
			rightLayout->addWidget(track3);
			rightLayout->addWidget(track4);
			rightLayout->addWidget(track5);
			rightLayout->addWidget(track6);
		}
		if (trackIndex >= 0) {
			rightLayout->addWidget(stream);
			rightLayout->addWidget(rec);
		}

		meterLayout->setContentsMargins(0, 0, 0, 0);
		meterLayout->setSpacing(0);
		meterLayout->addSpacing(4);
		meterLayout->addWidget(volMeter);
		meterLayout->addWidget(slider);
		meterLayout->addLayout(rightLayout);

		controlLayout->setContentsMargins(0, 0, 0, 0);
		controlLayout->setSpacing(0);
		if (showConfig)
			controlLayout->addWidget(config);
		controlLayout->addSpacing(15);
		controlLayout->addWidget(mute);

		mainLayout->addItem(nameLayout);
		mainLayout->addItem(volLayout);
		mainLayout->addItem(meterLayout);
		mainLayout->addItem(controlLayout);

		volMeter->setFocusProxy(slider);

#ifdef __APPLE__ //thanks to SuslikV for pointing this
		mon->setProperty("themeID", "MacOnly");
		mute->setProperty("themeID", "MacOnly");
		if (trackIndex >= 0) {
			rec->setProperty("themeID", "MacOnly");
		}
#endif
		setMaximumWidth(110);
	} else {
		QHBoxLayout *volLayout = new QHBoxLayout;
		QHBoxLayout *textLayout = new QHBoxLayout;
		QHBoxLayout *botLayout = new QHBoxLayout;

		volMeter = new VolumeMeter(nullptr, obs_volmeter, false);
		slider = new SliderIgnoreScroll(Qt::Horizontal);

		textLayout->setContentsMargins(0, 0, 0, 0);
		textLayout->setSpacing(0);
#if defined(_WIN32) || defined(__APPLE__) || HAVE_PULSEAUDIO
		textLayout->addWidget(mon);
		if (trackIndex < 0) {
#ifdef __APPLE__
			textLayout->addItem(new QSpacerItem(15, 0));
#endif
			textLayout->addWidget(track1);
			textLayout->addWidget(track2);
			textLayout->addWidget(track3);
			textLayout->addWidget(track4);
			textLayout->addWidget(track5);
			textLayout->addWidget(track6);
		}
#endif
#ifdef __APPLE__
		textLayout->addItem(new QSpacerItem(13, 0));
		;
#endif
		if (trackIndex >= 0) {
			textLayout->addWidget(stream);
#ifdef __APPLE__
			textLayout->addItem(new QSpacerItem(15, 0));
#endif
			textLayout->addWidget(rec);
#ifdef __APPLE__
			textLayout->addItem(new QSpacerItem(15, 0));
#else
			textLayout->addItem(new QSpacerItem(5, 0));
#endif
		}
		textLayout->addWidget(nameLabel);
		textLayout->addStretch();
		textLayout->addWidget(volLabel);
		textLayout->setAlignment(nameLabel, Qt::AlignLeft);
		textLayout->setAlignment(volLabel, Qt::AlignRight);

		volLayout->addWidget(slider);
		volLayout->addWidget(mute);
		volLayout->setSpacing(5);

		botLayout->setContentsMargins(0, 0, 0, 0);
		botLayout->setSpacing(0);
		botLayout->addLayout(volLayout);

		if (showConfig)
			botLayout->addWidget(config);

		mainLayout->addItem(textLayout);
		mainLayout->addWidget(volMeter);
		mainLayout->addItem(botLayout);

		volMeter->setFocusProxy(slider);
	}

	setLayout(mainLayout);

	QFont font = nameLabel->font();
	font.setPointSize(font.pointSize() - 1);

	nameLabel->setText(sourceName);
	nameLabel->setFont(font);
	volLabel->setFont(font);

	slider->setMinimum(0);
	slider->setMaximum(int(FADER_PRECISION));

	/* mute button */
	bool muted = obs_source_muted(source);
	mute->setChecked(muted);
	SetMuted(muted);
	mute->setAccessibleName(QTStr("VolControl.Mute").arg(sourceName));
	obs_fader_add_callback(obs_fader, OBSVolumeChanged, this);
	obs_volmeter_add_callback(obs_volmeter, OBSVolumeLevel, this);

	if (source != nullptr)
		signal_handler_connect(obs_source_get_signal_handler(source),
				       "mute", OBSVolumeMuted, this);

	QWidget::connect(slider, SIGNAL(valueChanged(int)), this,
			 SLOT(SliderChanged(int)));
	QWidget::connect(mute, SIGNAL(clicked(bool)), this,
			 SLOT(SetMuted(bool)));
	/* monitoring button */
#if defined(_WIN32) || defined(__APPLE__) || HAVE_PULSEAUDIO
	mon->setHidden(!showMon);
	bool monON = false;
	enum obs_monitoring_type type =
		(obs_monitoring_type)obs_source_get_monitoring_type(source);
	if (type != OBS_MONITORING_TYPE_NONE)
		monON = true;
	mon->setChecked(monON);
	SetMon(monON);
	mon->setAccessibleName(QTStr("VolControl.Mon"));
	mon->setToolTip(QTStr("VolControl.Mon.Tooltip"));
	QWidget::connect(mon, SIGNAL(clicked(bool)), this, SLOT(SetMon(bool)));
	if (source != nullptr)
		signal_handler_connect(obs_source_get_signal_handler(source),
				       "monitoring_type",
				       OBSSourceMonitoringChanged, this);
#endif
	if (trackIndex >= 0) {
		bool onAir = false;
		int stream_index =
			config_get_int(main->Config(), "AdvOut", "TrackIndex");
		if (track_index == stream_index - 1)
			onAir = true;
		stream->setChecked(onAir);
		stream->setAccessibleName(QTStr("VolControl.Stream"));
		stream->setToolTip(
			QTStr("VolControl.Stream.Tooltip").arg(track_index + 1));
		QWidget::connect(stream, SIGNAL(clicked(bool)), this,
				 SLOT(SetStream(bool)));

		bool onRec = false;
		std::string RecMode =
			config_get_string(main->Config(), "AdvOut", "RecType");
		bool isStandard = RecMode.compare("Standard") == 0;
		if (isStandard) {
			int recbitmask = config_get_int(main->Config(),
							"AdvOut", "RecTracks");
			if (recbitmask & 1 << track_index)
				onRec = true;
		} else {
			int FFbitmask = config_get_int(main->Config(), "AdvOut",
						       "FFAudioMixes");
			if (FFbitmask & 1 << track_index)
				onRec = true;
		}
		rec->setChecked(onRec);
		rec->setAccessibleName(QTStr("VolControl.Rec").arg(sourceName));
		rec->setToolTip(
			QTStr("VolControl.Rec.Tooltip").arg(track_index + 1));

		QWidget::connect(rec, SIGNAL(clicked(bool)), this,
				 SLOT(SetRec(bool)));
	}
	/* tracks buttons for input mixer */
	if (trackIndex < 0) {
		mixers = obs_source_get_audio_mixers(source);
		track1->setChecked(mixers & (1 << 0));
		track2->setChecked(mixers & (1 << 1));
		track3->setChecked(mixers & (1 << 2));
		track4->setChecked(mixers & (1 << 3));
		track5->setChecked(mixers & (1 << 4));
		track6->setChecked(mixers & (1 << 5));
		track1->setObjectName(QString::fromUtf8("track1"));
		track2->setObjectName(QString::fromUtf8("track2"));
		track3->setObjectName(QString::fromUtf8("track3"));
		track4->setObjectName(QString::fromUtf8("track4"));
		track5->setObjectName(QString::fromUtf8("track5"));
		track6->setObjectName(QString::fromUtf8("track6"));
		QWidget::connect(track1, SIGNAL(clicked(bool)), this,
				 SLOT(track1Changed(bool)));
		QWidget::connect(track2, SIGNAL(clicked(bool)), this,
				 SLOT(track2Changed(bool)));
		QWidget::connect(track3, SIGNAL(clicked(bool)), this,
				 SLOT(track3Changed(bool)));
		QWidget::connect(track4, SIGNAL(clicked(bool)), this,
				 SLOT(track4Changed(bool)));
		QWidget::connect(track5, SIGNAL(clicked(bool)), this,
				 SLOT(track5Changed(bool)));
		QWidget::connect(track6, SIGNAL(clicked(bool)), this,
				 SLOT(track6Changed(bool)));

		if (source != nullptr)
			signal_handler_connect(
				obs_source_get_signal_handler(source),
				"audio_mixers", OBSSourceMixersChanged, this);
	}
	obs_fader_attach_source(obs_fader, source);
	obs_volmeter_attach_source(obs_volmeter, source);

	QString styleName = slider->style()->objectName();
	QStyle *style;
	style = QStyleFactory::create(styleName);
	if (!style) {
		style = new SliderAbsoluteSetStyle();
	} else {
		style = new SliderAbsoluteSetStyle(style);
	}

	style->setParent(slider);
	slider->setStyle(style);

	/* Call volume changed once to init the slider position and label */
	VolumeChanged();
}

VolControl::~VolControl()
{
	obs_fader_remove_callback(obs_fader, OBSVolumeChanged, this);
	obs_volmeter_remove_callback(obs_volmeter, OBSVolumeLevel, this);

	if (source != nullptr) {
		signal_handler_disconnect(obs_source_get_signal_handler(source),
					  "mute", OBSVolumeMuted, this);
		signal_handler_disconnect(obs_source_get_signal_handler(source),
					  "audio_mixers",
					  OBSSourceMixersChanged, this);
		signal_handler_disconnect(obs_source_get_signal_handler(source),
					  "monitoring_type",
					  OBSSourceMonitoringChanged, this);
	}
	obs_fader_destroy(obs_fader);
	obs_volmeter_destroy(obs_volmeter);
}

QColor VolumeMeter::getBackgroundNominalColor() const
{
	return backgroundNominalColor;
}

void VolumeMeter::setBackgroundNominalColor(QColor c)
{
	backgroundNominalColor = std::move(c);
}

QColor VolumeMeter::getBackgroundWarningColor() const
{
	return backgroundWarningColor;
}

void VolumeMeter::setBackgroundWarningColor(QColor c)
{
	backgroundWarningColor = std::move(c);
}

QColor VolumeMeter::getBackgroundErrorColor() const
{
	return backgroundErrorColor;
}

void VolumeMeter::setBackgroundErrorColor(QColor c)
{
	backgroundErrorColor = std::move(c);
}

QColor VolumeMeter::getForegroundNominalColor() const
{
	return foregroundNominalColor;
}

void VolumeMeter::setForegroundNominalColor(QColor c)
{
	foregroundNominalColor = std::move(c);
}

QColor VolumeMeter::getForegroundWarningColor() const
{
	return foregroundWarningColor;
}

void VolumeMeter::setForegroundWarningColor(QColor c)
{
	foregroundWarningColor = std::move(c);
}

QColor VolumeMeter::getForegroundErrorColor() const
{
	return foregroundErrorColor;
}

void VolumeMeter::setForegroundErrorColor(QColor c)
{
	foregroundErrorColor = std::move(c);
}

QColor VolumeMeter::getClipColor() const
{
	return clipColor;
}

void VolumeMeter::setClipColor(QColor c)
{
	clipColor = std::move(c);
}

QColor VolumeMeter::getMagnitudeColor() const
{
	return magnitudeColor;
}

void VolumeMeter::setMagnitudeColor(QColor c)
{
	magnitudeColor = std::move(c);
}

QColor VolumeMeter::getMajorTickColor() const
{
	return majorTickColor;
}

void VolumeMeter::setMajorTickColor(QColor c)
{
	majorTickColor = std::move(c);
}

QColor VolumeMeter::getMinorTickColor() const
{
	return minorTickColor;
}

void VolumeMeter::setMinorTickColor(QColor c)
{
	minorTickColor = std::move(c);
}

qreal VolumeMeter::getMinimumLevel() const
{
	return minimumLevel;
}

void VolumeMeter::setMinimumLevel(qreal v)
{
	minimumLevel = v;
}

qreal VolumeMeter::getWarningLevel() const
{
	return warningLevel;
}

void VolumeMeter::setWarningLevel(qreal v)
{
	warningLevel = v;
}

qreal VolumeMeter::getErrorLevel() const
{
	return errorLevel;
}

void VolumeMeter::setErrorLevel(qreal v)
{
	errorLevel = v;
}

qreal VolumeMeter::getClipLevel() const
{
	return clipLevel;
}

void VolumeMeter::setClipLevel(qreal v)
{
	clipLevel = v;
}

qreal VolumeMeter::getMinimumInputLevel() const
{
	return minimumInputLevel;
}

void VolumeMeter::setMinimumInputLevel(qreal v)
{
	minimumInputLevel = v;
}

qreal VolumeMeter::getPeakDecayRate() const
{
	return peakDecayRate;
}

void VolumeMeter::setPeakDecayRate(qreal v)
{
	peakDecayRate = v;
}

qreal VolumeMeter::getMagnitudeIntegrationTime() const
{
	return magnitudeIntegrationTime;
}

void VolumeMeter::setMagnitudeIntegrationTime(qreal v)
{
	magnitudeIntegrationTime = v;
}

qreal VolumeMeter::getPeakHoldDuration() const
{
	return peakHoldDuration;
}

void VolumeMeter::setPeakHoldDuration(qreal v)
{
	peakHoldDuration = v;
}

qreal VolumeMeter::getInputPeakHoldDuration() const
{
	return inputPeakHoldDuration;
}

void VolumeMeter::setInputPeakHoldDuration(qreal v)
{
	inputPeakHoldDuration = v;
}

void VolumeMeter::setPeakMeterType(enum obs_peak_meter_type peakMeterType)
{
	obs_volmeter_set_peak_meter_type(obs_volmeter, peakMeterType);
	switch (peakMeterType) {
	case TRUE_PEAK_METER:
		// For true-peak meters EBU has defined the Permitted Maximum,
		// taking into account the accuracy of the meter and further
		// processing required by lossy audio compression.
		//
		// The alignment level was not specified, but I've adjusted
		// it compared to a sample-peak meter. Incidentally Youtube
		// uses this new Alignment Level as the maximum integrated
		// loudness of a video.
		//
		//  * Permitted Maximum Level (PML) = -2.0 dBTP
		//  * Alignment Level (AL) = -13 dBTP
		setErrorLevel(-2.0);
		setWarningLevel(-13.0);
		break;

	case SAMPLE_PEAK_METER:
	default:
		// For a sample Peak Meter EBU has the following level
		// definitions, taking into account inaccuracies of this meter:
		//
		//  * Permitted Maximum Level (PML) = -9.0 dBFS
		//  * Alignment Level (AL) = -20.0 dBFS
		setErrorLevel(-9.0);
		setWarningLevel(-20.0);
		break;
	}
}

void VolumeMeter::mousePressEvent(QMouseEvent *event)
{
	setFocus(Qt::MouseFocusReason);
	event->accept();
}

void VolumeMeter::wheelEvent(QWheelEvent *event)
{
	QApplication::sendEvent(focusProxy(), event);
}

VolumeMeter::VolumeMeter(QWidget *parent, obs_volmeter_t *obs_volmeter,
			 bool vertical)
	: QWidget(parent), obs_volmeter(obs_volmeter), vertical(vertical)
{
	// Use a font that can be rendered small.
	tickFont = QFont("Arial");
	tickFont.setPixelSize(7);
	// Default meter color settings, they only show if
	// there is no stylesheet, do not remove.
	backgroundNominalColor.setRgb(0x26, 0x7f, 0x26); // Dark green
	backgroundWarningColor.setRgb(0x7f, 0x7f, 0x26); // Dark yellow
	backgroundErrorColor.setRgb(0x7f, 0x26, 0x26);   // Dark red
	foregroundNominalColor.setRgb(0x4c, 0xff, 0x4c); // Bright green
	foregroundWarningColor.setRgb(0xff, 0xff, 0x4c); // Bright yellow
	foregroundErrorColor.setRgb(0xff, 0x4c, 0x4c);   // Bright red
	clipColor.setRgb(0xff, 0xff, 0xff);              // Bright white
	magnitudeColor.setRgb(0x00, 0x00, 0x00);         // Black
	majorTickColor.setRgb(0xff, 0xff, 0xff);         // Black
	minorTickColor.setRgb(0xcc, 0xcc, 0xcc);         // Black
	minimumLevel = -60.0;                            // -60 dB
	warningLevel = -20.0;                            // -20 dB
	errorLevel = -9.0;                               //  -9 dB
	clipLevel = -0.5;                                //  -0.5 dB
	minimumInputLevel = -50.0;                       // -50 dB
	peakDecayRate = 11.76;                           //  20 dB / 1.7 sec
	magnitudeIntegrationTime = 0.3;                  //  99% in 300 ms
	peakHoldDuration = 20.0;                         //  20 seconds
	inputPeakHoldDuration = 1.0;                     //  1 second

	channels = (int)audio_output_get_channels(obs_get_audio());

	handleChannelCofigurationChange();
	updateTimerRef = updateTimer.toStrongRef();
	if (!updateTimerRef) {
		updateTimerRef = QSharedPointer<VolumeMeterTimer>::create();
		updateTimerRef->start(34);
		updateTimer = updateTimerRef;
	}
	if (vertical)
		setMinimumHeight(50);
	updateTimerRef->AddVolControl(this);
}

VolumeMeter::~VolumeMeter()
{
	updateTimerRef->RemoveVolControl(this);
	delete tickPaintCache;
}

void VolumeMeter::setLevels(const float magnitude[MAX_AUDIO_CHANNELS],
			    const float peak[MAX_AUDIO_CHANNELS],
			    const float inputPeak[MAX_AUDIO_CHANNELS])
{
	uint64_t ts = os_gettime_ns();
	QMutexLocker locker(&dataMutex);

	currentLastUpdateTime = ts;
	for (int channelNr = 0; channelNr < MAX_AUDIO_CHANNELS; channelNr++) {
		currentMagnitude[channelNr] = magnitude[channelNr];
		currentPeak[channelNr] = peak[channelNr];
		currentInputPeak[channelNr] = inputPeak[channelNr];
	}

	// In case there are more updates then redraws we must make sure
	// that the ballistics of peak and hold are recalculated.
	locker.unlock();
	calculateBallistics(ts);
}

inline void VolumeMeter::resetLevels()
{
	currentLastUpdateTime = 0;
	for (int channelNr = 0; channelNr < MAX_AUDIO_CHANNELS; channelNr++) {
		currentMagnitude[channelNr] = -M_INFINITE;
		currentPeak[channelNr] = -M_INFINITE;
		currentInputPeak[channelNr] = -M_INFINITE;

		displayMagnitude[channelNr] = -M_INFINITE;
		displayPeak[channelNr] = -M_INFINITE;
		displayPeakHold[channelNr] = -M_INFINITE;
		displayPeakHoldLastUpdateTime[channelNr] = 0;
		displayInputPeakHold[channelNr] = -M_INFINITE;
		displayInputPeakHoldLastUpdateTime[channelNr] = 0;
	}
}

inline void VolumeMeter::handleChannelCofigurationChange()
{
	QMutexLocker locker(&dataMutex);

	int currentNrAudioChannels = obs_volmeter_get_nr_channels(obs_volmeter);
	if (displayNrAudioChannels != currentNrAudioChannels) {
		displayNrAudioChannels = currentNrAudioChannels;

		// Make room for 3 pixels meter, with one pixel between each.
		// Then 9/13 pixels for ticks and numbers.
		if (vertical)
			setMinimumSize(displayNrAudioChannels * 4 + 14, 130);
		else
			setMinimumSize(130, displayNrAudioChannels * 4 + 8);

		resetLevels();
	}
}

inline bool VolumeMeter::detectIdle(uint64_t ts)
{
	double timeSinceLastUpdate = (ts - currentLastUpdateTime) * 0.000000001;
	if (timeSinceLastUpdate > 0.5) {
		resetLevels();
		return true;
	} else {
		return false;
	}
}

inline void
VolumeMeter::calculateBallisticsForChannel(int channelNr, uint64_t ts,
					   qreal timeSinceLastRedraw)
{
	if (currentPeak[channelNr] >= displayPeak[channelNr] ||
	    isnan(displayPeak[channelNr])) {
		// Attack of peak is immediate.
		displayPeak[channelNr] = currentPeak[channelNr];
	} else {
		// Decay of peak is 40 dB / 1.7 seconds for Fast Profile
		// 20 dB / 1.7 seconds for Medium Profile (Type I PPM)
		// 24 dB / 2.8 seconds for Slow Profile (Type II PPM)
		float decay = float(peakDecayRate * timeSinceLastRedraw);
		displayPeak[channelNr] = CLAMP(displayPeak[channelNr] - decay,
					       currentPeak[channelNr], 0);
	}

	if (currentPeak[channelNr] >= displayPeakHold[channelNr] ||
	    !isfinite(displayPeakHold[channelNr])) {
		// Attack of peak-hold is immediate, but keep track
		// when it was last updated.
		displayPeakHold[channelNr] = currentPeak[channelNr];
		displayPeakHoldLastUpdateTime[channelNr] = ts;
	} else {
		// The peak and hold falls back to peak
		// after 20 seconds.
		qreal timeSinceLastPeak =
			(uint64_t)(ts -
				   displayPeakHoldLastUpdateTime[channelNr]) *
			0.000000001;
		if (timeSinceLastPeak > peakHoldDuration) {
			displayPeakHold[channelNr] = currentPeak[channelNr];
			displayPeakHoldLastUpdateTime[channelNr] = ts;
		}
	}

	if (currentInputPeak[channelNr] >= displayInputPeakHold[channelNr] ||
	    !isfinite(displayInputPeakHold[channelNr])) {
		// Attack of peak-hold is immediate, but keep track
		// when it was last updated.
		displayInputPeakHold[channelNr] = currentInputPeak[channelNr];
		displayInputPeakHoldLastUpdateTime[channelNr] = ts;
	} else {
		// The peak and hold falls back to peak after 1 second.
		qreal timeSinceLastPeak =
			(uint64_t)(
				ts -
				displayInputPeakHoldLastUpdateTime[channelNr]) *
			0.000000001;
		if (timeSinceLastPeak > inputPeakHoldDuration) {
			displayInputPeakHold[channelNr] =
				currentInputPeak[channelNr];
			displayInputPeakHoldLastUpdateTime[channelNr] = ts;
		}
	}

	if (!isfinite(displayMagnitude[channelNr])) {
		// The statements in the else-leg do not work with
		// NaN and infinite displayMagnitude.
		displayMagnitude[channelNr] = currentMagnitude[channelNr];
	} else {
		// A VU meter will integrate to the new value to 99% in 300 ms.
		// The calculation here is very simplified and is more accurate
		// with higher frame-rate.
		float attack =
			float((currentMagnitude[channelNr] -
			       displayMagnitude[channelNr]) *
			      (timeSinceLastRedraw / magnitudeIntegrationTime) *
			      0.99);
		displayMagnitude[channelNr] =
			CLAMP(displayMagnitude[channelNr] + attack,
			      (float)minimumLevel, 0);
	}
}

inline void VolumeMeter::calculateBallistics(uint64_t ts,
					     qreal timeSinceLastRedraw)
{
	QMutexLocker locker(&dataMutex);

	for (int channelNr = 0; channelNr < MAX_AUDIO_CHANNELS; channelNr++)
		calculateBallisticsForChannel(channelNr, ts,
					      timeSinceLastRedraw);
}

void VolumeMeter::paintInputMeter(QPainter &painter, int x, int y, int width,
				  int height, float peakHold)
{
	QMutexLocker locker(&dataMutex);
	QColor color;

	if (peakHold < minimumInputLevel)
		color = backgroundNominalColor;
	else if (peakHold < warningLevel)
		color = foregroundNominalColor;
	else if (peakHold < errorLevel)
		color = foregroundWarningColor;
	else if (peakHold <= clipLevel)
		color = foregroundErrorColor;
	else
		color = clipColor;

	painter.fillRect(x, y, width, height, color);
}

void VolumeMeter::paintHTicks(QPainter &painter, int x, int y, int width,
			      int height)
{
	qreal scale = width / minimumLevel;

	painter.setFont(tickFont);
	painter.setPen(majorTickColor);

	// Draw major tick lines and numeric indicators.
	for (int i = 0; i >= minimumLevel; i -= 5) {
		int position = int(x + width - (i * scale) - 1);
		QString str = QString::number(i);

		if (i == 0 || i == -5)
			painter.drawText(position - 3, height, str);
		else
			painter.drawText(position - 5, height, str);
		painter.drawLine(position, y, position, y + 2);
	}

	// Draw minor tick lines.
	painter.setPen(minorTickColor);
	for (int i = 0; i >= minimumLevel; i--) {
		int position = int(x + width - (i * scale) - 1);
		if (i % 5 != 0)
			painter.drawLine(position, y, position, y + 1);
	}
}

void VolumeMeter::paintVTicks(QPainter &painter, int x, int y, int height)
{
	qreal scale = height / minimumLevel;

	painter.setFont(tickFont);
	painter.setPen(majorTickColor);

	// Draw major tick lines and numeric indicators.
	for (int i = 0; i >= minimumLevel; i -= 5) {
		int position = y + int((i * scale) - 1);
		QString str = QString::number(i);

		if (i == 0)
			painter.drawText(x + 5, position + 4, str);
		else if (i == -60)
			painter.drawText(x + 4, position, str);
		else
			painter.drawText(x + 4, position + 2, str);
		painter.drawLine(x, position, x + 2, position);
	}

	// Draw minor tick lines.
	painter.setPen(minorTickColor);
	for (int i = 0; i >= minimumLevel; i--) {
		int position = y + int((i * scale) - 1);
		if (i % 5 != 0)
			painter.drawLine(x, position, x + 1, position);
	}
}

#define CLIP_FLASH_DURATION_MS 1000

void VolumeMeter::ClipEnding()
{
	clipping = false;
}

void VolumeMeter::paintHMeter(QPainter &painter, int x, int y, int width,
			      int height, float magnitude, float peak,
			      float peakHold)
{
	qreal scale = width / minimumLevel;

	QMutexLocker locker(&dataMutex);
	int minimumPosition = x + 0;
	int maximumPosition = x + width;
	int magnitudePosition = int(x + width - (magnitude * scale));
	int peakPosition = int(x + width - (peak * scale));
	int peakHoldPosition = int(x + width - (peakHold * scale));
	int warningPosition = int(x + width - (warningLevel * scale));
	int errorPosition = int(x + width - (errorLevel * scale));

	int nominalLength = warningPosition - minimumPosition;
	int warningLength = errorPosition - warningPosition;
	int errorLength = maximumPosition - errorPosition;
	locker.unlock();

	if (clipping) {
		peakPosition = maximumPosition;
	}

	if (peakPosition < minimumPosition) {
		painter.fillRect(minimumPosition, y, nominalLength, height,
				 backgroundNominalColor);
		painter.fillRect(warningPosition, y, warningLength, height,
				 backgroundWarningColor);
		painter.fillRect(errorPosition, y, errorLength, height,
				 backgroundErrorColor);
	} else if (peakPosition < warningPosition) {
		painter.fillRect(minimumPosition, y,
				 peakPosition - minimumPosition, height,
				 foregroundNominalColor);
		painter.fillRect(peakPosition, y,
				 warningPosition - peakPosition, height,
				 backgroundNominalColor);
		painter.fillRect(warningPosition, y, warningLength, height,
				 backgroundWarningColor);
		painter.fillRect(errorPosition, y, errorLength, height,
				 backgroundErrorColor);
	} else if (peakPosition < errorPosition) {
		painter.fillRect(minimumPosition, y, nominalLength, height,
				 foregroundNominalColor);
		painter.fillRect(warningPosition, y,
				 peakPosition - warningPosition, height,
				 foregroundWarningColor);
		painter.fillRect(peakPosition, y, errorPosition - peakPosition,
				 height, backgroundWarningColor);
		painter.fillRect(errorPosition, y, errorLength, height,
				 backgroundErrorColor);
	} else if (peakPosition < maximumPosition) {
		painter.fillRect(minimumPosition, y, nominalLength, height,
				 foregroundNominalColor);
		painter.fillRect(warningPosition, y, warningLength, height,
				 foregroundWarningColor);
		painter.fillRect(errorPosition, y, peakPosition - errorPosition,
				 height, foregroundErrorColor);
		painter.fillRect(peakPosition, y,
				 maximumPosition - peakPosition, height,
				 backgroundErrorColor);
	} else {
		if (!clipping) {
			QTimer::singleShot(CLIP_FLASH_DURATION_MS, this,
					   SLOT(ClipEnding()));
			clipping = true;
		}

		int end = errorLength + warningLength + nominalLength;
		painter.fillRect(minimumPosition, y, end, height,
				 QBrush(foregroundErrorColor));
	}

	if (peakHoldPosition - 3 < minimumPosition)
		; // Peak-hold below minimum, no drawing.
	else if (peakHoldPosition < warningPosition)
		painter.fillRect(peakHoldPosition - 3, y, 3, height,
				 foregroundNominalColor);
	else if (peakHoldPosition < errorPosition)
		painter.fillRect(peakHoldPosition - 3, y, 3, height,
				 foregroundWarningColor);
	else
		painter.fillRect(peakHoldPosition - 3, y, 3, height,
				 foregroundErrorColor);

	if (magnitudePosition - 3 >= minimumPosition)
		painter.fillRect(magnitudePosition - 3, y, 3, height,
				 magnitudeColor);
}

void VolumeMeter::paintVMeter(QPainter &painter, int x, int y, int width,
			      int height, float magnitude, float peak,
			      float peakHold)
{
	qreal scale = height / minimumLevel;

	QMutexLocker locker(&dataMutex);
	int minimumPosition = y + 0;
	int maximumPosition = y + height;
	int magnitudePosition = int(y + height - (magnitude * scale));
	int peakPosition = int(y + height - (peak * scale));
	int peakHoldPosition = int(y + height - (peakHold * scale));
	int warningPosition = int(y + height - (warningLevel * scale));
	int errorPosition = int(y + height - (errorLevel * scale));

	int nominalLength = warningPosition - minimumPosition;
	int warningLength = errorPosition - warningPosition;
	int errorLength = maximumPosition - errorPosition;
	locker.unlock();

	if (clipping) {
		peakPosition = maximumPosition;
	}

	if (peakPosition < minimumPosition) {
		painter.fillRect(x, minimumPosition, width, nominalLength,
				 backgroundNominalColor);
		painter.fillRect(x, warningPosition, width, warningLength,
				 backgroundWarningColor);
		painter.fillRect(x, errorPosition, width, errorLength,
				 backgroundErrorColor);
	} else if (peakPosition < warningPosition) {
		painter.fillRect(x, minimumPosition, width,
				 peakPosition - minimumPosition,
				 foregroundNominalColor);
		painter.fillRect(x, peakPosition, width,
				 warningPosition - peakPosition,
				 backgroundNominalColor);
		painter.fillRect(x, warningPosition, width, warningLength,
				 backgroundWarningColor);
		painter.fillRect(x, errorPosition, width, errorLength,
				 backgroundErrorColor);
	} else if (peakPosition < errorPosition) {
		painter.fillRect(x, minimumPosition, width, nominalLength,
				 foregroundNominalColor);
		painter.fillRect(x, warningPosition, width,
				 peakPosition - warningPosition,
				 foregroundWarningColor);
		painter.fillRect(x, peakPosition, width,
				 errorPosition - peakPosition,
				 backgroundWarningColor);
		painter.fillRect(x, errorPosition, width, errorLength,
				 backgroundErrorColor);
	} else if (peakPosition < maximumPosition) {
		painter.fillRect(x, minimumPosition, width, nominalLength,
				 foregroundNominalColor);
		painter.fillRect(x, warningPosition, width, warningLength,
				 foregroundWarningColor);
		painter.fillRect(x, errorPosition, width,
				 peakPosition - errorPosition,
				 foregroundErrorColor);
		painter.fillRect(x, peakPosition, width,
				 maximumPosition - peakPosition,
				 backgroundErrorColor);
	} else {
		if (!clipping) {
			QTimer::singleShot(CLIP_FLASH_DURATION_MS, this,
					   SLOT(ClipEnding()));
			clipping = true;
		}

		int end = errorLength + warningLength + nominalLength;
		painter.fillRect(x, minimumPosition, width, end,
				 QBrush(foregroundErrorColor));
	}

	if (peakHoldPosition - 3 < minimumPosition)
		; // Peak-hold below minimum, no drawing.
	else if (peakHoldPosition < warningPosition)
		painter.fillRect(x, peakHoldPosition - 3, width, 3,
				 foregroundNominalColor);
	else if (peakHoldPosition < errorPosition)
		painter.fillRect(x, peakHoldPosition - 3, width, 3,
				 foregroundWarningColor);
	else
		painter.fillRect(x, peakHoldPosition - 3, width, 3,
				 foregroundErrorColor);

	if (magnitudePosition - 3 >= minimumPosition)
		painter.fillRect(x, magnitudePosition - 3, width, 3,
				 magnitudeColor);
}

void VolumeMeter::paintEvent(QPaintEvent *event)
{
	uint64_t ts = os_gettime_ns();
	qreal timeSinceLastRedraw = (ts - lastRedrawTime) * 0.000000001;

	const QRect rect = event->region().boundingRect();
	int width = rect.width();
	int height = rect.height();

	handleChannelCofigurationChange();
	calculateBallistics(ts, timeSinceLastRedraw);
	bool idle = detectIdle(ts);

	// Draw the ticks in a off-screen buffer when the widget changes size.
	QSize tickPaintCacheSize;
	if (vertical)
		tickPaintCacheSize = QSize(14, height);
	else
		tickPaintCacheSize = QSize(width, 9);
	if (tickPaintCache == nullptr ||
	    tickPaintCache->size() != tickPaintCacheSize) {
		delete tickPaintCache;
		tickPaintCache = new QPixmap(tickPaintCacheSize);

		QColor clearColor(0, 0, 0, 0);
		tickPaintCache->fill(clearColor);

		QPainter tickPainter(tickPaintCache);
		if (vertical) {
			tickPainter.translate(0, height);
			tickPainter.scale(1, -1);
			paintVTicks(tickPainter, 0, 11,
				    tickPaintCacheSize.height() - 11);
		} else {
			paintHTicks(tickPainter, 6, 0,
				    tickPaintCacheSize.width() - 6,
				    tickPaintCacheSize.height());
		}
		tickPainter.end();
	}

	// Actual painting of the widget starts here.
	QPainter painter(this);
	if (vertical) {
		// Invert the Y axis to ease the math
		painter.translate(0, height);
		painter.scale(1, -1);
		painter.drawPixmap(displayNrAudioChannels * 4 - 1, 7,
				   *tickPaintCache);
	} else {
		painter.drawPixmap(0, height - 9, *tickPaintCache);
	}

	for (int channelNr = 0; channelNr < displayNrAudioChannels;
	     channelNr++) {

		int channelNrFixed =
			(displayNrAudioChannels == 1 && channels > 2)
				? 2
				: channelNr;

		if (vertical)
			paintVMeter(painter, channelNr * 4, 8, 3, height - 10,
				    displayMagnitude[channelNrFixed],
				    displayPeak[channelNrFixed],
				    displayPeakHold[channelNrFixed]);
		else
			paintHMeter(painter, 5, channelNr * 4, width - 5, 3,
				    displayMagnitude[channelNrFixed],
				    displayPeak[channelNrFixed],
				    displayPeakHold[channelNrFixed]);

		if (idle)
			continue;

		// By not drawing the input meter boxes the user can
		// see that the audio stream has been stopped, without
		// having too much visual impact.
		if (vertical)
			paintInputMeter(painter, channelNr * 4, 3, 3, 3,
					displayInputPeakHold[channelNrFixed]);
		else
			paintInputMeter(painter, 0, channelNr * 4, 3, 3,
					displayInputPeakHold[channelNrFixed]);
	}

	lastRedrawTime = ts;
}

void VolumeMeterTimer::AddVolControl(VolumeMeter *meter)
{
	volumeMeters.push_back(meter);
}

void VolumeMeterTimer::RemoveVolControl(VolumeMeter *meter)
{
	volumeMeters.removeOne(meter);
}

void VolumeMeterTimer::timerEvent(QTimerEvent *)
{
	for (VolumeMeter *meter : volumeMeters)
		meter->update();
}
